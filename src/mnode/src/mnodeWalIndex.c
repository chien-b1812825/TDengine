/*
 * Copyright (c) 2019 TAOS Data, Inc. <cli@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "tfile.h"
#include "cache.h"
#include "hash.h"
#include "mnodeSdb.h"
#include "mnodeInt.h"
#include "twal.h"
#include "tglobal.h"
#include "mnodeSdbTable.h"
#include "mnodeWalIndex.h"

typedef struct walIndexItem {
  struct walIndexItem* prev;
  struct walIndexItem* next;

  walIndex index;
} walIndexItem;

typedef struct walIndexFileInfo {
  char name[TSDB_FILENAME_LEN];

  uint64_t   version;
  int64_t offset;
  int64_t total;

  walIndexItem* tail[SDB_TABLE_MAX];
  walIndexItem* head[SDB_TABLE_MAX];

  int64_t tableSize[SDB_TABLE_MAX];

  SHashObj* pItemTable[SDB_TABLE_MAX];
} walIndexFileInfo;

int16_t nWalFileInfo;
walIndexFileInfo* tsWalFileInfo;
char* tsWalIndexBuffer;

// index data type

typedef enum walIndexContentType {
  WAL_INDEX_HEADER        = 0,
  WAL_INDEX_TABLE_HEADER  = 1,
  WAL_INDEX_ITEM          = 2,
}walIndexContentType;

#pragma  pack(1)
typedef struct walIndexHeaderMeta {
  walIndexContentType type;
  int64_t totalSize;
  uint64_t version;
  int64_t offset;
  int32_t walNameSize;
} walIndexHeaderMeta;

#pragma  pack(1)
typedef struct walIndexHeader {
  walIndexHeaderMeta meta;

  char walName[];
} walIndexHeader;

#pragma  pack(1)
typedef struct walIndexTableHeader {
  walIndexContentType type;
  ESdbTable tableId;
  int64_t tableSize;
} walIndexTableHeader;

static int32_t buildIndex(void *wparam, void *hparam, int32_t qtype, void *tparam, void* head) {
  //SSdbRow *pRow = wparam;
  SWalHead *pHead = hparam;
  SWalHeadInfo* pHeadInfo = (SWalHeadInfo*)head;
  int32_t tableId = pHead->msgType / 10;
  int32_t action = pHead->msgType % 10;
  walIndexFileInfo *pFileInfo = tsWalFileInfo;
  ESdbKey keyType = sdbGetKeyTypeByTableId(tableId);

  SHashObj* pTable = pFileInfo->pItemTable[tableId];
  strcpy(pFileInfo->name, pHeadInfo->name);

  int32_t keySize;
  void* key = sdbTableGetKeyAndSize(keyType, pHead->cont, &keySize, false);

  walIndexItem* pItem = calloc(1, sizeof(walIndexItem) + keySize);
  *pItem = (walIndexItem) {
    .index = (walIndex) {
      .keyLen = keySize,
      .offset = pHeadInfo->offset,
      .size   = pHeadInfo->len,
    },
    .next = NULL,
    .prev = NULL,
  };
  memcpy(pItem->index.key, key, keySize);  

  if (action == SDB_ACTION_UPDATE || action == SDB_ACTION_DELETE) {
    walIndexItem** ppItem = (walIndexItem**)taosHashGet(pTable, key, keySize);
    assert(ppItem != NULL);

    walIndexItem* prev = (*ppItem)->prev;
    walIndexItem* next = (*ppItem)->next;

    if (pFileInfo->tail[tableId] == *ppItem) {
      pFileInfo->tail[tableId] = prev;
    }
    if (pFileInfo->head[tableId] == *ppItem) {
      pFileInfo->head[tableId] = next;
    }
    if (prev) prev->next = next;
    if (next) next->prev = prev;    

    pFileInfo->total -= sizeof(walIndex) + (*ppItem)->index.keyLen;
    pFileInfo->tableSize[tableId] -= sizeof(walIndex) + (*ppItem)->index.keyLen;
    assert(pFileInfo->total >= 0);
    free(*ppItem);
  }

  if (action == SDB_ACTION_INSERT || action == SDB_ACTION_UPDATE) {
    if (pFileInfo->tail[tableId] == NULL) {      
      pFileInfo->tail[tableId] = pItem;      
    } else {
      pItem->prev = tsWalFileInfo[0].tail[tableId];
      pFileInfo->tail[tableId]->next = pItem;
      pFileInfo->tail[tableId] = pItem;
    }
    if (pFileInfo->head[tableId] == NULL) {
      pFileInfo->head[tableId] = pItem;   
    }

    taosHashPut(pTable, key, keySize, &pItem, sizeof(walIndexItem**));
    pFileInfo->total += sizeof(walIndex) + keySize;
    pFileInfo->tableSize[tableId] += sizeof(walIndex) + keySize;
  }

  if (pHeadInfo->offset + pHeadInfo->len > pFileInfo->offset) {
    pFileInfo->offset = pHeadInfo->offset + pHeadInfo->len;
  }

  if (pHead->version > pFileInfo->version) {
    pFileInfo->version = pHead->version;
  }

  return 0;
}

void mnodeSdbBuildWalIndex(void* handle) {
  nWalFileInfo = 0;
  tsWalFileInfo = calloc(1, sizeof(walIndexFileInfo));
  walIndexFileInfo *pFileInfo = tsWalFileInfo;
  char* save = NULL;

  int i = 0;
  for (i = 0; i < SDB_TABLE_MAX; ++i) {
    ESdbKey keyType = sdbGetKeyTypeByTableId(i);
    _hash_fn_t hashFp = taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT);
    if (keyType == SDB_KEY_STRING || keyType == SDB_KEY_VAR_STRING) {
      hashFp = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
    }
    pFileInfo->pItemTable[i] = taosHashInit(1024, hashFp, true, HASH_ENTRY_LOCK);
  }

  int32_t code = walRestore(handle, NULL, buildIndex);
  if (code != 0) {
    goto _err;
  }
    
  assert(sizeof(walIndexHeaderMeta) == sizeof(walIndexContentType) + sizeof(int64_t)*2 + sizeof(uint64_t) + sizeof(int32_t) );
  int64_t headerSize = sizeof(walIndexHeaderMeta) + strlen(pFileInfo->name);
  
  int64_t indexTotal = pFileInfo->total;
  int64_t tableHeaderTotal = SDB_TABLE_MAX*(sizeof(walIndexTableHeader));
  int64_t total = headerSize + indexTotal + tableHeaderTotal;
  char *buffer = calloc(1, total);  
  if (buffer == NULL) {    
    goto _err;
  }
  save = buffer;

  // save header
  walIndexHeaderMeta *pIndexHeader = (walIndexHeaderMeta*)buffer;
  *pIndexHeader = (walIndexHeaderMeta) {
    .type         = WAL_INDEX_HEADER,
    .totalSize    = indexTotal + tableHeaderTotal,
    .version      = pFileInfo->version,
    .offset       = pFileInfo->offset,
    .walNameSize  = strlen(pFileInfo->name),
  };
  buffer += sizeof(walIndexHeaderMeta);
  memcpy(buffer, pFileInfo->name, strlen(pFileInfo->name)); buffer += strlen(pFileInfo->name);

  // build table info
  for (i = 0; i < SDB_TABLE_MAX; ++i) {
    // build table header
    walIndexTableHeader *pTableHeader = (walIndexTableHeader*)buffer;
    *pTableHeader = (walIndexTableHeader) {
      .type = WAL_INDEX_TABLE_HEADER,
      .tableId = i,
      .tableSize = pFileInfo->tableSize[i],
    };
    buffer += sizeof(walIndexTableHeader);

    char* pTableBegin = buffer;

    // build index item array
    walIndexItem* pItem = pFileInfo->head[i];
    while (pItem) {
      memcpy(buffer, &pItem->index, sizeof(walIndex) + pItem->index.keyLen);
      buffer += sizeof(walIndex) + pItem->index.keyLen;
      pItem = pItem->next;
    }

    assert((buffer - pTableBegin) == pFileInfo->tableSize[i]);
  }
  assert((buffer - save) == total);

  char    name[TSDB_FILENAME_LEN] = {0};
  sprintf(name, "%s/wal/index", tsMnodeDir);
  remove(name);
  int64_t tfd = tfOpenM(name, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
  if (!tfValid(tfd)) {
    sdbError("file:%s, failed to open for build index since %s", name, strerror(errno));
    goto _err;
  }

  tfWrite(tfd, save, total);
  tfFsync(tfd);
  tfClose(tfd);
  sdbInfo("file:%s, open for build index", name);

_err:
  tfree(save);
  for (i = 0; i < SDB_TABLE_MAX; ++i) {
    walIndexItem* pItem = pFileInfo->head[i];
    while (pItem) {
      walIndexItem* pNext = pItem->next;
      free(pItem);
      pItem = pNext;
    }
    
    taosHashCleanup(pFileInfo->pItemTable[i]);
  }
  free(tsWalFileInfo);
  tsWalFileInfo = NULL;
}

int64_t sdbRestoreFromIndex(FWalIndexReader fpReader) {
  char    name[TSDB_FILENAME_LEN] = {0};
  sprintf(name, "%s/wal/index", tsMnodeDir);
  int64_t offset = -1;

  int64_t tfd = tfOpen(name, O_RDONLY);
  if (!tfValid(tfd)) {
    sdbError("file:%s, failed to open index since %s", name, strerror(errno));
    goto _err;
  }

  int64_t size = tfLseek(tfd, 0, SEEK_END);
  if (size < 0) {
    goto _err;
  }
  char *buffer = calloc(1, size);
  if (buffer == NULL) {
    goto _err;
  }
  tfLseek(tfd, 0, SEEK_SET);
  if (tfRead(tfd, buffer, size) != size) {
    sdbError("file:%s, failed to read since %s", name, strerror(errno));
    goto _err;
  }

  tsWalIndexBuffer = buffer;
  
  while (true) {
    // read header
    walIndexHeader *pHeader = (walIndexHeader*)buffer;
    assert(pHeader->meta.type == WAL_INDEX_HEADER);
    buffer += sizeof(walIndexHeaderMeta) + pHeader->meta.walNameSize;
    memset(name, 0, sizeof(name));
    memcpy(name, pHeader->walName, pHeader->meta.walNameSize);

    int64_t total = pHeader->meta.totalSize;
    uint64_t walVersion = pHeader->meta.version;

    int64_t fd = tfOpen(name, O_RDONLY);
    if (!tfValid(fd)) {
      sdbError("file:%s, failed to open for build index since %s", name, strerror(errno));
      goto _err;
    }

    // read table header
    while (total > 0) {
      walIndexTableHeader *pTableHeader = (walIndexTableHeader*)buffer;
      assert(pTableHeader->type == WAL_INDEX_TABLE_HEADER);

      ESdbTable tableId = pTableHeader->tableId;
      int64_t tableSize = pTableHeader->tableSize;
      buffer += sizeof(walIndexTableHeader);

      total -= sizeof(walIndexTableHeader);

      int64_t readBytes = 0;
      while (readBytes < tableSize) {
        walIndex* pIndex = (walIndex*)buffer;
        buffer += sizeof(walIndex) + pIndex->keyLen;
        readBytes += sizeof(walIndex) + pIndex->keyLen;

        fpReader(fd, name, tableId, walVersion, pIndex);
      }
      assert(readBytes == tableSize);

      total -= readBytes;
    }
    assert(total == 0);
    break;
  }

_err:
  tfClose(tfd);
  tfree(tsWalIndexBuffer);
  return offset;
}
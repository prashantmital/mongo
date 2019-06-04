/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>
#include <third_party/murmurhash3/MurmurHash3.h>

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

namespace {

// TODO SERVER-36385: Completely remove the key size check in 4.4
bool largeKeyDisallowed() {
    return (serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40);
}

// The number of items we can scan before we must yield.
static const int kScanLimit = 1000;
static const size_t kNumHashBuckets = 1U << 16;

}  // namespace

IndexConsistency::IndexConsistency(OperationContext* opCtx,
                                   Collection* collection,
                                   NamespaceString nss,
                                   RecordStore* recordStore,
                                   bool background)
    : _opCtx(opCtx),
      _collection(collection),
      _nss(nss),
      _recordStore(recordStore),
      _tracker(opCtx->getServiceContext()->getFastClockSource(),
               internalQueryExecYieldIterations.load(),
               Milliseconds(internalQueryExecYieldPeriodMS.load())),
      _firstPhase(true) {
    _indexKeyCount.resize(kNumHashBuckets);

    IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    std::unique_ptr<IndexCatalog::IndexIterator> indexIterator =
        indexCatalog->getIndexIterator(_opCtx, false);

    while (indexIterator->more()) {
        const IndexDescriptor* descriptor = indexIterator->next()->descriptor();
        std::string indexName = descriptor->indexName();
        _indexNumber[indexName] = _indexesInfo.size();

        IndexInfo indexInfo;

        indexInfo.descriptor = descriptor;
        indexInfo.isReady = DurableCatalog::get(opCtx)->isIndexReady(opCtx, nss, indexName);

        uint32_t indexNameHash;
        MurmurHash3_x86_32(indexName.c_str(), indexName.size(), 0, &indexNameHash);
        indexInfo.indexNameHash = indexNameHash;
        indexInfo.indexScanFinished = false;

        indexInfo.numKeys = 0;
        indexInfo.numLongKeys = 0;
        indexInfo.numRecords = 0;

        _indexesInfo.push_back(indexInfo);
    }
}

void IndexConsistency::addMultikeyMetadataPath(const KeyString& ks, IndexInfo* indexInfo) {
    indexInfo->hashedMultikeyMetadataPaths.emplace(_hashKeyString(ks, indexInfo->indexNameHash));
}

void IndexConsistency::removeMultikeyMetadataPath(const KeyString& ks, IndexInfo* indexInfo) {
    indexInfo->hashedMultikeyMetadataPaths.erase(_hashKeyString(ks, indexInfo->indexNameHash));
}

size_t IndexConsistency::getMultikeyMetadataPathCount(IndexInfo* indexInfo) {
    return indexInfo->hashedMultikeyMetadataPaths.size();
}

void IndexConsistency::addLongIndexKey(IndexInfo* indexInfo) {
    indexInfo->numRecords++;
    indexInfo->numLongKeys++;
}

bool IndexConsistency::haveEntryMismatch() const {
    return std::any_of(
        _indexKeyCount.begin(), _indexKeyCount.end(), [](int count) -> bool { return count; });
}

void IndexConsistency::setSecondPhase() {
    invariant(_firstPhase);
    _firstPhase = false;
}

void IndexConsistency::addIndexEntryErrors(ValidateResultsMap* indexNsResultsMap,
                                           ValidateResults* results) {
    invariant(!_firstPhase);

    // We'll report up to 1MB for extra index entry errors and missing index entry errors.
    const int kErrorSizeMB = 1 * 1024 * 1024;
    int numMissingIndexEntriesSizeMB = 0;
    int numExtraIndexEntriesSizeMB = 0;

    int numMissingIndexEntryErrors = _missingIndexEntries.size();
    int numExtraIndexEntryErrors = 0;
    for (const auto& item : _extraIndexEntries) {
        numExtraIndexEntryErrors += item.second.size();
    }

    // Inform which indexes have inconsistences and add the BSON objects of the inconsistent index
    // entries to the results vector.
    bool missingIndexEntrySizeLimitWarning = false;
    for (const auto& missingIndexEntry : _missingIndexEntries) {
        const BSONObj& entry = missingIndexEntry.second;

        // Only count the indexKey and idKey fields towards the total size.
        numMissingIndexEntriesSizeMB += entry["indexKey"].size();
        if (entry.hasField("idKey")) {
            numMissingIndexEntriesSizeMB += entry["idKey"].size();
        }

        if (numMissingIndexEntriesSizeMB <= kErrorSizeMB) {
            results->missingIndexEntries.push_back(entry);
        } else if (!missingIndexEntrySizeLimitWarning) {
            StringBuilder ss;
            ss << "Not all missing index entry inconsistencies are listed due to size limitations.";
            results->errors.push_back(ss.str());

            missingIndexEntrySizeLimitWarning = true;
        }

        std::string indexName = entry["indexName"].String();
        if (!indexNsResultsMap->at(indexName).valid) {
            continue;
        }

        StringBuilder ss;
        ss << "Index with name '" << indexName << "' has inconsistencies.";
        results->errors.push_back(ss.str());

        indexNsResultsMap->at(indexName).valid = false;
    }

    bool extraIndexEntrySizeLimitWarning = false;
    for (const auto& extraIndexEntry : _extraIndexEntries) {
        const SimpleBSONObjSet& entries = extraIndexEntry.second;
        for (const auto& entry : entries) {
            // Only count the indexKey field towards the total size.
            numExtraIndexEntriesSizeMB += entry["indexKey"].size();
            if (numExtraIndexEntriesSizeMB <= kErrorSizeMB) {
                results->extraIndexEntries.push_back(entry);
            } else if (!extraIndexEntrySizeLimitWarning) {
                StringBuilder ss;
                ss << "Not all extra index entry inconsistencies are listed due to size "
                      "limitations.";
                results->errors.push_back(ss.str());

                extraIndexEntrySizeLimitWarning = true;
            }

            std::string indexName = entry["indexName"].String();
            if (!indexNsResultsMap->at(indexName).valid) {
                continue;
            }

            StringBuilder ss;
            ss << "Index with name '" << indexName << "' has inconsistencies.";
            results->errors.push_back(ss.str());

            indexNsResultsMap->at(indexName).valid = false;
        }
    }

    // Inform how many inconsistencies were detected.
    if (numMissingIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numMissingIndexEntryErrors << " missing index entries.";
        results->warnings.push_back(ss.str());
    }

    if (numExtraIndexEntryErrors > 0) {
        StringBuilder ss;
        ss << "Detected " << numExtraIndexEntryErrors << " extra index entries.";
        results->warnings.push_back(ss.str());
    }

    results->valid = false;
}

void IndexConsistency::addDocKey(const KeyString& ks,
                                 IndexInfo* indexInfo,
                                 RecordId recordId,
                                 const BSONObj& indexKey) {
    // Ignore indexes that weren't ready before we started validation.
    if (!indexInfo->isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexInfo->indexNameHash);

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the document
        // keys encountered.
        _indexKeyCount[hash]++;
        indexInfo->numRecords++;
    } else if (_indexKeyCount[hash]) {
        // Found a document key for a hash bucket that had mismatches.

        // Get the documents _id index key.
        auto cursor = _recordStore->getCursor(_opCtx);
        auto record = cursor->seekExact(recordId);
        invariant(record);

        BSONObj data = record->data.toBson();
        boost::optional<BSONElement> idKey = boost::none;
        if (data.hasField("_id")) {
            idKey = data["_id"];
        }

        std::string key = std::string(ks.getBuffer(), ks.getSize());
        BSONObj info = _generateInfo(*indexInfo, recordId, indexKey, idKey);

        // Cannot have duplicate KeyStrings during the document scan phase.
        invariant(_missingIndexEntries.count(key) == 0);
        _missingIndexEntries.insert(std::make_pair(key, info));
    }
}

void IndexConsistency::addIndexKey(const KeyString& ks,
                                   IndexInfo* indexInfo,
                                   RecordId recordId,
                                   const BSONObj& indexKey) {
    // Ignore indexes that weren't ready before we started validation.
    if (!indexInfo->isReady) {
        return;
    }

    const uint32_t hash = _hashKeyString(ks, indexInfo->indexNameHash);

    if (_firstPhase) {
        // During the first phase of validation we only keep track of the count for the index entry
        // keys encountered.
        _indexKeyCount[hash]--;
        indexInfo->numKeys++;
    } else if (_indexKeyCount[hash]) {
        // Found an index key for a bucket that has inconsistencies.
        // If there is a corresponding document key for the index entry key, we remove the key from
        // the '_missingIndexEntries' map. However if there was no document key for the index entry
        // key, we add the key to the '_extraIndexEntries' map.

        std::string key = std::string(ks.getBuffer(), ks.getSize());
        BSONObj info = _generateInfo(*indexInfo, recordId, indexKey, boost::none);

        if (_missingIndexEntries.count(key) == 0) {
            // We may have multiple extra index entries for a given KeyString.
            auto search = _extraIndexEntries.find(key);
            if (search == _extraIndexEntries.end()) {
                SimpleBSONObjSet infoSet = {info};
                _extraIndexEntries.insert(std::make_pair(key, infoSet));
                return;
            }

            search->second.insert(info);
        } else {
            _missingIndexEntries.erase(key);
        }
    }
}

BSONObj IndexConsistency::_generateInfo(const IndexInfo& indexInfo,
                                        RecordId recordId,
                                        const BSONObj& indexKey,
                                        boost::optional<BSONElement> idKey) {
    const std::string& indexName = indexInfo.descriptor->indexName();
    const BSONObj& keyPattern = indexInfo.descriptor->keyPattern();

    // We need to rehydrate the indexKey for improved readability.
    // {"": ObjectId(...)} -> {"_id": ObjectId(...)}
    auto keysIt = keyPattern.begin();
    auto valuesIt = indexKey.begin();

    BSONObjBuilder b;
    while (keysIt != keyPattern.end()) {
        // keysIt and valuesIt must have the same number of elements.
        invariant(valuesIt != indexKey.end());
        b.appendAs(*valuesIt, keysIt->fieldName());
        keysIt++;
        valuesIt++;
    }

    BSONObj rehydratedKey = b.done();

    if (idKey) {
        return BSON("indexName" << indexName << "recordId" << recordId.repr() << "idKey" << *idKey
                                << "indexKey" << rehydratedKey);
    } else {
        return BSON("indexName" << indexName << "recordId" << recordId.repr() << "indexKey"
                                << rehydratedKey);
    }
}

uint32_t IndexConsistency::_hashKeyString(const KeyString& ks, uint32_t indexNameHash) const {
    MurmurHash3_x86_32(
        ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize(), indexNameHash, &indexNameHash);
    MurmurHash3_x86_32(ks.getBuffer(), ks.getSize(), indexNameHash, &indexNameHash);
    return indexNameHash % kNumHashBuckets;
}
}  // namespace mongo

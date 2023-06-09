/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * File Created: Thursday, 6th September 2018 10:49:53 am
 * Author: yangyaokai
 */
#include <fcntl.h>
#include <algorithm>
#include <memory>

#include "src/chunkserver/datastore/chunkserver_datastore.h"
#include "src/chunkserver/datastore/chunkserver_chunkfile.h"
#include "src/common/crc32.h"
#include "src/common/curve_define.h"

namespace curve {
namespace chunkserver {

namespace {

bool ValidMinIoAlignment(const char* /*flagname*/, uint32_t value) {
    return common::is_aligned(value, 512);
}

}  // namespace

DEFINE_uint32(minIoAlignment, 512,
              "minimum alignment for io request, must align to 512");

DEFINE_validator(minIoAlignment, ValidMinIoAlignment);

ChunkFileMetaPage::ChunkFileMetaPage(const ChunkFileMetaPage& metaPage) {
    version = metaPage.version;
    sn = metaPage.sn;
    correctedSn = metaPage.correctedSn;
    location = metaPage.location;
    if (metaPage.bitmap != nullptr) {
        bitmap = std::make_shared<Bitmap>(metaPage.bitmap->Size(),
                                          metaPage.bitmap->GetBitmap());
    } else {
        bitmap = nullptr;
    }
}

ChunkFileMetaPage& ChunkFileMetaPage::operator =(
    const ChunkFileMetaPage& metaPage) {
    if (this == &metaPage)
        return *this;
    version = metaPage.version;
    sn = metaPage.sn;
    correctedSn = metaPage.correctedSn;
    location = metaPage.location;
    if (metaPage.bitmap != nullptr) {
        bitmap = std::make_shared<Bitmap>(metaPage.bitmap->Size(),
                                          metaPage.bitmap->GetBitmap());
    } else {
        bitmap = nullptr;
    }
    return *this;
}

void ChunkFileMetaPage::encode(char* buf) {
    size_t len = 0;
    memcpy(buf, &version, sizeof(version));
    len += sizeof(version);
    memcpy(buf + len, &sn, sizeof(sn));
    len += sizeof(sn);
    memcpy(buf + len, &correctedSn, sizeof(correctedSn));
    len += sizeof(correctedSn);
    size_t loc_size = location.size();
    memcpy(buf + len, &loc_size, sizeof(loc_size));
    len += sizeof(loc_size);
    // CloneChunk need serialized location information and bitmap information
    if (loc_size > 0) {
        memcpy(buf + len, location.c_str(), loc_size);
        len += loc_size;
        uint32_t bits = bitmap->Size();
        memcpy(buf + len, &bits, sizeof(bits));
        len += sizeof(bits);
        size_t bitmapBytes = (bits + 8 - 1) >> 3;
        memcpy(buf + len, bitmap->GetBitmap(), bitmapBytes);
        len += bitmapBytes;
    }
    uint32_t crc = ::curve::common::CRC32(buf, len);
    memcpy(buf + len, &crc, sizeof(crc));
}

CSErrorCode ChunkFileMetaPage::decode(const char* buf) {
    size_t len = 0;
    memcpy(&version, buf, sizeof(version));
    len += sizeof(version);
    memcpy(&sn, buf + len, sizeof(sn));
    len += sizeof(sn);
    memcpy(&correctedSn, buf + len, sizeof(correctedSn));
    len += sizeof(correctedSn);
    size_t loc_size;
    memcpy(&loc_size, buf + len, sizeof(loc_size));
    len += sizeof(loc_size);
    if (loc_size > 0) {
        location = string(buf + len, loc_size);
        len += loc_size;
        uint32_t bits = 0;
        memcpy(&bits, buf + len, sizeof(bits));
        len += sizeof(bits);
        bitmap = std::make_shared<Bitmap>(bits, buf + len);
        size_t bitmapBytes = (bitmap->Size() + 8 - 1) >> 3;
        len += bitmapBytes;
    }
    uint32_t crc =  ::curve::common::CRC32(buf, len);
    uint32_t recordCrc;
    memcpy(&recordCrc, buf + len, sizeof(recordCrc));
    // check crc
    if (crc != recordCrc) {
        LOG(ERROR) << "Checking Crc32 failed.";
        return CSErrorCode::CrcCheckError;
    }

    // TODO(yyk) check version compatibility, currrent simple error handing,
    // need detailed implementation later
    if (!(version == FORMAT_VERSION || version == FORMAT_VERSION_V2)) {
        LOG(ERROR) << "File format version incompatible."
                   << "file version: " << version
                   << ", valid version: [" << FORMAT_VERSION
                   << ", " << FORMAT_VERSION_V2 << "]";
        return CSErrorCode::IncompatibleError;
    }
    return CSErrorCode::Success;
}

uint64_t CSChunkFile::syncChunkLimits_ = 2 * 1024 * 1024;
uint64_t CSChunkFile::syncThreshold_ = 64 * 1024;

CSChunkFile::CSChunkFile(std::shared_ptr<LocalFileSystem> lfs,
                         std::shared_ptr<FilePool> chunkFilePool,
                         const ChunkOptions& options)
    : cvar_(nullptr),
      chunkrate_(nullptr),
      fd_(-1),
      size_(options.chunkSize),
      pageSize_(options.pageSize),
      chunkId_(options.id),
      baseDir_(options.baseDir),
      isCloneChunk_(false),
      snapshots_(std::make_shared<CSSnapshots>(options.pageSize)),
      chunkFilePool_(chunkFilePool),
      lfs_(lfs),
      metric_(options.metric),
      enableOdsyncWhenOpenChunkFile_(options.enableOdsyncWhenOpenChunkFile) {
    CHECK(!baseDir_.empty()) << "Create chunk file failed";
    CHECK(lfs_ != nullptr) << "Create chunk file failed";
    metaPage_.sn = options.sn;
    metaPage_.correctedSn = options.correctedSn;
    metaPage_.location = options.location;
    // If location is not empty, it is CloneChunk,
    //     and Bitmap needs to be initialized
    if (!metaPage_.location.empty()) {
        uint32_t bits = size_ / pageSize_;
        metaPage_.bitmap = std::make_shared<Bitmap>(bits);
        isCloneChunk_ = true;
    }
    if (metric_ != nullptr) {
        metric_->chunkFileCount << 1;
    }
}

CSChunkFile::~CSChunkFile() {
    if (fd_ >= 0) {
        lfs_->Close(fd_);
    }

    if (metric_ != nullptr) {
        metric_->chunkFileCount << -1;
        if (isCloneChunk_) {
            metric_->cloneChunkCount << -1;
        }
    }
}

CSErrorCode CSChunkFile::Open(bool createFile) {
    WriteLockGuard writeGuard(rwLock_);
    string chunkFilePath = path();
    // Create a new file, if the chunk file already exists, no need to create
    // The existence of chunk files may be caused by two situations:
    // 1. getchunk succeeded, but failed in stat or load metapage last time;
    // 2. Two write requests concurrently create new chunk files
    if (createFile
        && !lfs_->FileExists(chunkFilePath)
        && metaPage_.sn > 0) {

        std::unique_ptr<char[]> buf(new char[pageSize_]);
        memset(buf.get(), 0, pageSize_);
        metaPage_.version = FORMAT_VERSION_V2;
        metaPage_.encode(buf.get());

        int rc = chunkFilePool_->GetFile(chunkFilePath, buf.get(), true);
        // When creating files concurrently, the previous thread may have been
        // created successfully, then -EEXIST will be returned here. At this
        // point, you can continue to open the generated file
        // But the current operation of the same chunk is serial, this problem
        // will not occur
        if (rc != 0  && rc != -EEXIST) {
            LOG(ERROR) << "Error occured when create file."
                       << " filepath = " << chunkFilePath;
            return CSErrorCode::InternalError;
        }
    }
    int rc = -1;
    if (enableOdsyncWhenOpenChunkFile_) {
        rc = lfs_->Open(chunkFilePath, O_RDWR|O_NOATIME|O_DSYNC);
    } else {
        rc = lfs_->Open(chunkFilePath, O_RDWR|O_NOATIME);
    }
    if (rc < 0) {
        LOG(ERROR) << "Error occured when opening file."
                   << " filepath = " << chunkFilePath;
        return CSErrorCode::InternalError;
    }
    fd_ = rc;
    struct stat fileInfo;
    rc = lfs_->Fstat(fd_, &fileInfo);
    if (rc < 0) {
        LOG(ERROR) << "Error occured when stating file."
                   << " filepath = " << chunkFilePath;
        return CSErrorCode::InternalError;
    }

    if (fileInfo.st_size != fileSize()) {
        LOG(ERROR) << "Wrong file size."
                   << " filepath = " << chunkFilePath
                   << ", real filesize = " << fileInfo.st_size
                   << ", expect filesize = " << fileSize();
        return CSErrorCode::FileFormatError;
    }

    CSErrorCode errCode = loadMetaPage();
    // After restarting, only after reopening and loading the metapage,
    // can we know whether it is a clone chunk
    if (!metaPage_.location.empty() && !isCloneChunk_) {
        if (metric_ != nullptr) {
            metric_->cloneChunkCount << 1;
        }
        isCloneChunk_ = true;
    }

    // if the cloneNo is not 0, the set the isCloneChunk_
    if (metaPage_.cloneNo != 0) {
        if (metric_ != nullptr) {
            metric_->cloneChunkCount << 1;
        }
        isCloneChunk_ = true;
    }
    
    return errCode;
}

//To open clone chunk file
CSErrorCode CSChunkFile::Open(bool createFile, uint64_t cloneNo) {
    WriteLockGuard writeGuard(rwLock_);
    string chunkFilePath = path(cloneNo);
    // Create a new file, if the chunk file already exists, no need to create
    // The existence of chunk files may be caused by two situations:
    // 1. getchunk succeeded, but failed in stat or load metapage last time;
    // 2. Two write requests concurrently create new chunk files
    if (createFile
        && !lfs_->FileExists(chunkFilePath)
        && metaPage_.sn > 0) {

        std::unique_ptr<char[]> buf(new char[pageSize_]);
        memset(buf.get(), 0, pageSize_);
        metaPage_.version = FORMAT_VERSION_V2;
        metaPage_.encode(buf.get());

        int rc = chunkFilePool_->GetFile(chunkFilePath, buf.get(), true);

        // When creating files concurrently, the previous thread may have been
        // created successfully, then -EEXIST will be returned here. At this
        // point, you can continue to open the generated file
        // But the current operation of the same chunk is serial, this problem
        // will not occur
        if (rc != 0  && rc != -EEXIST) {
            LOG(ERROR) << "Error occured when create file."
                       << " filepath = " << chunkFilePath;
            return CSErrorCode::InternalError;
        }
    }
    int rc = -1;
    if (enableOdsyncWhenOpenChunkFile_) {
        rc = lfs_->Open(chunkFilePath, O_RDWR|O_NOATIME|O_DSYNC);
    } else {
        rc = lfs_->Open(chunkFilePath, O_RDWR|O_NOATIME);
    }
    if (rc < 0) {
        LOG(ERROR) << "Error occured when opening file."
                   << " filepath = " << chunkFilePath;
        return CSErrorCode::InternalError;
    }
    fd_ = rc;
    struct stat fileInfo;
    rc = lfs_->Fstat(fd_, &fileInfo);
    if (rc < 0) {
        LOG(ERROR) << "Error occured when stating file."
                   << " filepath = " << chunkFilePath;
        return CSErrorCode::InternalError;
    }

    if (fileInfo.st_size != fileSize()) {
        LOG(ERROR) << "Wrong file size."
                   << " filepath = " << chunkFilePath
                   << ", real filesize = " << fileInfo.st_size
                   << ", expect filesize = " << fileSize();
        return CSErrorCode::FileFormatError;
    }

    CSErrorCode errCode = loadMetaPage();
    // After restarting, only after reopening and loading the metapage,
    // can we know whether it is a clone chunk
    if (!metaPage_.location.empty() && !isCloneChunk_) {
        if (metric_ != nullptr) {
            metric_->cloneChunkCount << 1;
        }
        isCloneChunk_ = true;
    }

    // if the parentID is not 0, the set the isCloneChunk_
    if (metaPage_.cloneNo != 0) {
        if (metric_ != nullptr) {
            metric_->cloneChunkCount << 1;
        }
        isCloneChunk_ = true;
    }
    
    return errCode;
}

CSErrorCode CSChunkFile::LoadSnapshot(SequenceNum sn) {
    WriteLockGuard writeGuard(rwLock_);
    return loadSnapshot(sn);
}

CSErrorCode CSChunkFile::loadSnapshot(SequenceNum sn) {
    if (snapshots_->contains(sn)) {
        LOG(ERROR) << "Multiple snapshot file found with same SeqNum."
                   << " ChunkID: " << chunkId_
                   << " Snapshot sn: " << sn;
        return CSErrorCode::SnapshotConflictError;
    }
    ChunkOptions options;
    options.id = chunkId_;
    options.sn = sn;
    options.baseDir = baseDir_;
    options.chunkSize = size_;
    options.pageSize = pageSize_;
    options.metric = metric_;
    CSSnapshot *snapshot_ = new(std::nothrow) CSSnapshot(lfs_,
                                            chunkFilePool_,
                                            options);
    CHECK(snapshot_ != nullptr) << "Failed to new CSSnapshot!"
                                << "ChunkID:" << chunkId_
                                << ",snapshot sn:" << sn;
    CSErrorCode errorCode = snapshot_->Open(false);
    if (errorCode != CSErrorCode::Success) {
        delete snapshot_;
        snapshot_ = nullptr;
        LOG(ERROR) << "Load snapshot failed."
                   << "ChunkID: " << chunkId_
                   << ",snapshot sn: " << sn;
        return errorCode;
    }
    snapshots_->insert(snapshot_);
    return errorCode;
}

CSErrorCode CSChunkFile::Write(SequenceNum sn,
                               const butil::IOBuf& buf,
                               off_t offset,
                               size_t length,
                               uint32_t* cost,
                               std::shared_ptr<SnapContext> ctx) {
    WriteLockGuard writeGuard(rwLock_);
    if (!CheckOffsetAndLength(
            offset, length, isCloneChunk_ ? pageSize_ : FLAGS_minIoAlignment)) {
        LOG(ERROR) << "Write chunk failed, invalid offset or length."
                   << "ChunkID: " << chunkId_
                   << ", offset: " << offset
                   << ", length: " << length
                   << ", page size: " << pageSize_
                   << ", chunk size: " << size_
                   << ", align: "
                   << (isCloneChunk_ ? pageSize_ : FLAGS_minIoAlignment);
        return CSErrorCode::InvalidArgError;
    }
    // Curve will ensure that all previous requests arrive or time out
    // before issuing new requests after user initiate a snapshot request.
    // Therefore, this is only a log recovery request, and it must have been
    // executed, and an error code can be returned here.
    if (sn < metaPage_.sn || sn < metaPage_.correctedSn) {
        LOG(WARNING) << "Backward write request."
                     << "ChunkID: " << chunkId_
                     << ",request sn: " << sn
                     << ",chunk sn: " << metaPage_.sn
                     << ",correctedSn: " << metaPage_.correctedSn;
        return CSErrorCode::BackwardRequestError;
    }
    if (snapshots_->getCurrentSn() != 0 && ctx->empty()) {
        LOG(ERROR) << "Exists old snapshot sn: " << snapshots_->getCurrentSn()
                   << ", but snapshot context is empty.";
            return CSErrorCode::SnapshotConflictError;
        }
    // Determine whether to create a snapshot file
    if (needCreateSnapshot(sn, ctx)) {
        CSErrorCode err = createSnapshot(ctx->getLatest());
        if (err != CSErrorCode::Success) {
            return err;
        }
        DLOG(INFO) << "Create snapshotChunk success, "
                   << "ChunkID: " << chunkId_
                   << ",request sn: " << sn
                   << ",chunk sn: " << metaPage_.sn;
    }
    // If the requested sequence number is greater than the current chunk
    // sequence number, the metapage needs to be updated
    if (sn > metaPage_.sn) {
        ChunkFileMetaPage tempMeta = metaPage_;
        tempMeta.sn = sn;
        CSErrorCode errorCode = updateMetaPage(&tempMeta);
        if (errorCode != CSErrorCode::Success) {
            LOG(ERROR) << "Update metapage failed."
                       << "ChunkID: " << chunkId_
                       << ",request sn: " << sn
                       << ",chunk sn: " << metaPage_.sn;
            return errorCode;
        }
        metaPage_.sn = tempMeta.sn;
    }
    // If it is cow, copy the data to the snapshot file first
    if (needCow(sn, ctx)) {
        CSErrorCode errorCode = copy2Snapshot(offset, length);
        if (errorCode != CSErrorCode::Success) {
            LOG(ERROR) << "Copy data to snapshot failed."
                        << "ChunkID: " << chunkId_
                        << ",request sn: " << sn
                        << ",chunk sn: " << metaPage_.sn;
            return errorCode;
        }
    }
    int rc = writeData(buf, offset, length);
    if (rc < 0) {
        LOG(ERROR) << "Write data to chunk file failed."
                   << "ChunkID: " << chunkId_
                   << ",request sn: " << sn
                   << ",chunk sn: " << metaPage_.sn;
        return CSErrorCode::InternalError;
    }
    // If it is a clone chunk, the bitmap will be updated
    CSErrorCode errorCode = flush();
    if (errorCode != CSErrorCode::Success) {
        LOG(ERROR) << "Write data to chunk file failed."
                   << "ChunkID: " << chunkId_
                   << ",request sn: " << sn
                   << ",chunk sn: " << metaPage_.sn;
        return errorCode;
    }

    if (chunkrate_.get() && cvar_.get()) {
        *chunkrate_ += length;
        uint64_t res = *chunkrate_;
        // if single write size > syncThreshold, for cache friend to
        // delay to sync.
        auto actualSyncChunkLimits = MayUpdateWriteLimits(res);
        if (*chunkrate_ >= actualSyncChunkLimits &&
                chunkrate_->compare_exchange_weak(res, 0)) {
            cvar_->notify_one();
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::createSnapshot(SequenceNum sn) {
    // clone chunk does not allow to create snapshot
    if (isCloneChunk_) {
        LOG(ERROR) << "Clone chunk can't create snapshot."
                   << "ChunkID: " << chunkId_
                   << ",request sn: " << sn
                   << ",chunk sn: " << metaPage_.sn;
        return CSErrorCode::StatusConflictError;
    }

    if (snapshots_->contains(sn)) {
        return CSErrorCode::Success;
    }

    // create snapshot
    ChunkOptions options;
    options.id = chunkId_;
    options.sn = sn;
    options.baseDir = baseDir_;
    options.chunkSize = size_;
    options.pageSize = pageSize_;
    options.metric = metric_;
    auto snapshot_ = new (std::nothrow) CSSnapshot(lfs_,
                                                   chunkFilePool_,
                                                   options);
    CHECK(snapshot_ != nullptr) << "Failed to new CSSnapshot!";
    CSErrorCode errorCode = snapshot_->Open(true);
    if (errorCode != CSErrorCode::Success) {
        delete snapshot_;
        snapshot_ = nullptr;
        LOG(ERROR) << "Create snapshot failed."
                   << "ChunkID: " << chunkId_
                   << ",request sn: " << sn
                   << ",chunk sn: " << metaPage_.sn;
        return errorCode;
    }

    snapshots_->insert(snapshot_);
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::Sync() {
    WriteLockGuard writeGuard(rwLock_);
    int rc = SyncData();
    if (rc < 0) {
        LOG(ERROR) << "Sync data failed, "
                   << "ChunkID:" << chunkId_;
        return CSErrorCode::InternalError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::Paste(const char * buf, off_t offset, size_t length) {
    WriteLockGuard writeGuard(rwLock_);
    // If it is not a clone chunk, return success directly
    if (!isCloneChunk_) {
        return CSErrorCode::Success;
    }
    if (!CheckOffsetAndLength(offset, length, pageSize_)) {
        LOG(ERROR) << "Paste chunk failed, invalid offset or length."
                   << "ChunkID: " << chunkId_
                   << ", offset: " << offset
                   << ", length: " << length
                   << ", page size: " << pageSize_
                   << ", chunk size: " << size_
                   << ", align: " << pageSize_;
        return CSErrorCode::InvalidArgError;
    }

    // The request above must be pagesize aligned
    // the starting page index number of the paste area
    uint32_t beginIndex = offset / pageSize_;
    // the last page index number of the paste area
    uint32_t endIndex = (offset + length - 1) / pageSize_;
    // Get the unwritten range of the current file
    std::vector<BitRange> uncopiedRange;
    metaPage_.bitmap->Divide(beginIndex,
                             endIndex,
                             &uncopiedRange,
                             nullptr);

    // For the unwritten range, write the corresponding data
    off_t pasteOff;
    size_t pasteSize;
    for (auto& range : uncopiedRange) {
        pasteOff = range.beginIndex * pageSize_;
        pasteSize = (range.endIndex - range.beginIndex + 1) * pageSize_;
        int rc = writeData(buf + (pasteOff - offset), pasteOff, pasteSize);
        if (rc < 0) {
            LOG(ERROR) << "Paste data to chunk failed."
                       << "ChunkID: " << chunkId_
                       << ", offset: " << offset
                       << ", length: " << length;
            return CSErrorCode::InternalError;
        }
    }

    // Update bitmap
    CSErrorCode errorCode = flush();
    if (errorCode != CSErrorCode::Success) {
        LOG(ERROR) << "Paste data to chunk failed."
                    << "ChunkID: " << chunkId_
                    << ", offset: " << offset
                    << ", length: " << length;
        return errorCode;
    }
    return CSErrorCode::Success;
}

CSChunkFile* CSChunkFile::getClone (uint64_t cloneNo) {
    return clonesMap_[cloneNo];
}

void CSChunkFile::addClone (uint64_t cloneNo, CSChunkFile* clone) {
    clonesMap_[cloneNo] = clone;
}

bool CSChunkFile::DivideObjInfoByIndex (SequenceNum sn, std::vector<BitRange>& range, std::vector<BitRange>& notInRanges, 
                                        std::vector<ObjectInfo>& objInfos) {

    bool isFinish = false;
    if (sn > 0) {
        isFinish = DivideSnapshotObjInfoByIndex(sn, range, notInRanges, objInfos);
        if (true == isFinish) {
            return true;
        }
    }

    if (nullptr == metaPage_.bitmap) { //not bitmap means that this chunk is not clone chunk
        for (auto& r : notInRanges) {
            ObjectInfo objInfo;
            objInfo.fileptr = std::shared_ptr<CSChunkFile>(this);
            objInfo.sn = sn;
            objInfo.snapptr = nullptr;
            objInfo.index = r.beginIndex;
            objInfo.offset = r.beginIndex << OBJ_SIZE_SHIFT;
            objInfo.length = (r.endIndex - r.beginIndex + 1) << OBJ_SIZE_SHIFT;
            objInfos.push_back(objInfo);
        }

        return true;
    }

    std::vector<BitRange> setRanges;
    std::vector<BitRange> clearRanges;
    std::vector<BitRange> dataRanges;

    if (sn > 0) {
        dataRanges = notInRanges;
    } else {
        dataRanges = range;
    }
    
    notInRanges.clear();
    for(auto& r : dataRanges) {
        setRanges.clear();
        clearRanges.clear();

        metaPage_.bitmap->Divide(r.beginIndex, r.endIndex, &clearRanges, &setRanges);
        for (auto& tmpc : clearRanges) {
            notInRanges.push_back (tmpc);
        }

        for (auto& tmpr : setRanges) {
            ObjectInfo objInfo;
            objInfo.fileptr = std::shared_ptr<CSChunkFile>(this);
            objInfo.sn = sn;
            objInfo.snapptr = nullptr;
            objInfo.index = tmpr.beginIndex;
            objInfo.offset = tmpr.beginIndex << OBJ_SIZE_SHIFT;
            objInfo.length = (tmpr.endIndex - tmpr.beginIndex + 1) << OBJ_SIZE_SHIFT;
            objInfos.push_back(objInfo);
        }
    }

    if (notInRanges.empty()) {
        isFinish = true;
    }

    return isFinish;
}

bool CSChunkFile::DivideSnapshotObjInfoByIndex (SequenceNum sn, std::vector<BitRange>& range, 
                                                std::vector<BitRange>& notInRanges, 
                                                std::vector<ObjectInfo>& objInfos) {
    return snapshots_->DivideSnapshotObjInfoByIndex(std::shared_ptr<CSChunkFile>(this), sn, range, notInRanges, objInfos);
}

//Just read data from specified snapshot
CSErrorCode CSChunkFile::ReadSpecifiedSnap (SequenceNum sn, CSSnapshotPtr snap, 
                                            char* buff, off_t offset, size_t length) {

    CSErrorCode rc;

    if (nullptr == snap) {
        rc = ReadSpecifiedChunk (sn, buff, offset, length);
        if (rc != CSErrorCode::Success) {
            LOG(ERROR) << "Read specified chunk failed."
                       << "ChunkID: " << chunkId_
                       << ",chunk sn: " << metaPage_.sn;
            return rc;
        }
    }

    ReadLockGuard readGuard(rwLock_);

    rc = snap->Read(buff, offset, length);
    if (rc != CSErrorCode::Success) {
        LOG(ERROR) << "Read specified snapshot failed."
                   << "ChunkID: " << chunkId_
                   << ",chunk sn: " << metaPage_.sn;
        return rc;
    }

    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::Read(char * buf, off_t offset, size_t length) {
    ReadLockGuard readGuard(rwLock_);
    if (!CheckOffsetAndLength(
            offset, length, isCloneChunk_ ? pageSize_ : FLAGS_minIoAlignment)) {
        LOG(ERROR) << "Read chunk failed, invalid offset or length."
                   << "ChunkID: " << chunkId_
                   << ", offset: " << offset
                   << ", length: " << length
                   << ", page size: " << pageSize_
                   << ", chunk size: " << size_
                   << ", align: "
                   << (isCloneChunk_ ? pageSize_ : FLAGS_minIoAlignment);
        return CSErrorCode::InvalidArgError;
    }

    // If it is clonechunk, ensure that the read area has been written,
    // otherwise an error is returned
    if (isCloneChunk_) {
        // The request above must be pagesize aligned
        // the starting page index number of the paste area
        uint32_t beginIndex = offset / pageSize_;
        // the last page index number of the paste area
        uint32_t endIndex = (offset + length - 1) / pageSize_;
        if (metaPage_.bitmap->NextClearBit(beginIndex, endIndex)
            != Bitmap::NO_POS) {
            LOG(ERROR) << "Read chunk file failed, has page never written."
                       << "ChunkID: " << chunkId_
                       << ", offset: " << offset
                       << ", length: " << length;
            return CSErrorCode::PageNerverWrittenError;
        }
    }

    int rc = readData(buf, offset, length);
    if (rc < 0) {
        LOG(ERROR) << "Read chunk file failed."
                   << "ChunkID: " << chunkId_
                   << ",chunk sn: " << metaPage_.sn;
        return CSErrorCode::InternalError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::ReadMetaPage(char * buf) {
    ReadLockGuard readGuard(rwLock_);
    int rc = readMetaPage(buf);
    if (rc < 0) {
        LOG(ERROR) << "Read chunk meta page failed."
                   << "ChunkID: " << chunkId_
                   << ",chunk sn: " << metaPage_.sn;
        return CSErrorCode::InternalError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::ReadSpecifiedChunk(SequenceNum sn,
                                            char * buf,
                                            off_t offset,
                                            size_t length)  {
    ReadLockGuard readGuard(rwLock_);
    if (!CheckOffsetAndLength(offset, length, pageSize_)) {
        LOG(ERROR) << "Read specified chunk failed, invalid offset or length."
                   << "ChunkID: " << chunkId_
                   << ", offset: " << offset
                   << ", length: " << length
                   << ", page size: " << pageSize_
                   << ", chunk size: " << size_
                   << ", align: " << pageSize_;
        return CSErrorCode::InvalidArgError;
    }
    // If the sequence equals the sequence of the current chunk,
    // read the current chunk file
    if (sn == metaPage_.sn) {
        int rc = readData(buf, offset, length);
        if (rc < 0) {
            LOG(ERROR) << "Read chunk file failed."
                       << "ChunkID: " << chunkId_
                       << ",chunk sn: " << metaPage_.sn;
            return CSErrorCode::InternalError;
        }
        return CSErrorCode::Success;
    }

    std::vector<BitRange> uncopiedRange;
    CSErrorCode errCode = snapshots_->Read(sn, buf, offset, length, &uncopiedRange);
    if (errCode != CSErrorCode::Success) {
        return errCode;
    }

    errCode = CSErrorCode::Success;
    off_t readOff;
    size_t readSize;
    // For uncopied extents, read chunk data
    for (auto& range : uncopiedRange) {
        readOff = range.beginIndex * pageSize_;
        readSize = (range.endIndex - range.beginIndex + 1) * pageSize_;
        int rc = readData(buf + (readOff - offset),
                          readOff,
                          readSize);
        if (rc < 0) {
            LOG(ERROR) << "Read chunk file failed. "
                       << "ChunkID: " << chunkId_
                       << ", chunk sn: " << metaPage_.sn;
            return CSErrorCode::InternalError;
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::Delete(SequenceNum sn)  {
    WriteLockGuard writeGuard(rwLock_);
    // If sn is less than the current sequence of the chunk, can not be deleted
    if (sn < metaPage_.sn) {
        LOG(WARNING) << "Delete chunk failed, backward request."
                     << "ChunkID: " << chunkId_
                     << ", request sn: " << sn
                     << ", chunk sn: " << metaPage_.sn;
        return CSErrorCode::BackwardRequestError;
    }

    // There should be no snapshots
    if (snapshots_->getCurrentSn() != 0) {
        LOG(WARNING) << "Delete chunk not allowed. There is snapshot."
                       << "ChunkID: " << chunkId_
                     << ", request sn: " << sn
                     << ", snapshot sn: " << snapshots_->getCurrentSn();
        return CSErrorCode::SnapshotExistError;
    }

    if (fd_ >= 0) {
        lfs_->Close(fd_);
        fd_ = -1;
    }
    int ret = chunkFilePool_->RecycleFile(path());
    if (ret < 0)
        return CSErrorCode::InternalError;

    LOG(INFO) << "Chunk deleted."
              << "ChunkID: " << chunkId_
              << ", request sn: " << sn
              << ", chunk sn: " << metaPage_.sn;
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::DeleteSnapshot(SequenceNum snapSn, std::shared_ptr<SnapContext> ctx) {
    WriteLockGuard writeGuard(rwLock_);

    // If it is a clone chunk, theoretically this interface should not be called
    if (isCloneChunk_) {
        LOG(ERROR) << "Delete snapshot failed, this is a clone chunk."
                   << "ChunkID: " << chunkId_;
        return CSErrorCode::StatusConflictError;
    }

    /*
     * If chunk.sn>snap.sn, then this snapshot is either a historical snapshot,
     * or a snapshot of the current sequence of the chunk,
     * in this case the snapshot is allowed to be deleted.
     * If chunk.sn<=snap.sn, then this snapshot must be generated after the
     * current delete operation. The current delete operation is the historical
     * log of playback, and deletion is not allowed in this case.
     */
    if(snapshots_->contains(snapSn) && metaPage_.sn > snapshots_->getCurrentSn()){
        return snapshots_->Delete(this, snapSn, ctx);
        }
    return CSErrorCode::Success;
}

void CSChunkFile::GetInfo(CSChunkInfo* info)  {
    ReadLockGuard readGuard(rwLock_);
    info->chunkId = chunkId_;
    info->pageSize = pageSize_;
    info->chunkSize = size_;
    info->curSn = metaPage_.sn;
    info->correctedSn = metaPage_.correctedSn;
    info->snapSn = snapshots_->getCurrentSn();
    info->isClone = isCloneChunk_;
    info->location = metaPage_.location;
    // There will be a memcpy, otherwise you need to lock the bitmap operation.
    // This step exists on the critical path of ReadChunk, which has certain
    // requirements for performance.
    // TODO(yyk) needs to evaluate which method performs better.
    if (metaPage_.bitmap != nullptr)
        info->bitmap = std::make_shared<Bitmap>(metaPage_.bitmap->Size(),
                                                metaPage_.bitmap->GetBitmap());
    else
        info->bitmap = nullptr;
}

CSErrorCode CSChunkFile::GetHash(off_t offset,
                                 size_t length,
                                 std::string* hash)  {
    ReadLockGuard readGuard(rwLock_);
    uint32_t crc32c = 0;

    char *buf = new(std::nothrow) char[length];
    if (nullptr == buf) {
        return CSErrorCode::InternalError;
    }

    int rc = lfs_->Read(fd_, buf, offset, length);
    if (rc < 0) {
        LOG(ERROR) << "Read chunk file failed."
                   << "ChunkID: " << chunkId_
                   << ",chunk sn: " << metaPage_.sn;
        delete[] buf;
        return CSErrorCode::InternalError;
    }

    crc32c = curve::common::CRC32(crc32c, buf, length);
    *hash = std::to_string(crc32c);

    delete[] buf;

    return CSErrorCode::Success;
}

bool CSChunkFile::needCreateSnapshot(SequenceNum sn, std::shared_ptr<SnapContext> ctx) {
    // ad-hoc hack.  clone chunk cannot create snapshot
    if (isCloneChunk_)
        return sn > std::max(metaPage_.correctedSn, metaPage_.sn);
    return !ctx->empty() && !snapshots_->contains(ctx->getLatest());
}

bool CSChunkFile::needCow(SequenceNum sn, std::shared_ptr<SnapContext> ctx) {
    // There is no snapshots thus no need to do cow
    if (ctx->empty())
        return false;

    SequenceNum chunkSn = std::max(ctx->getLatest(), metaPage_.sn);
    // Requests smaller than chunkSn will be rejected directly
    if (sn < chunkSn)
        return false;

    // The preceding logic ensures that the sn here must be equal to metaPage.sn
    // Because if sn<metaPage_.sn, the request will be rejected
    // When sn>metaPage_.sn, metaPage.sn will be updated to sn first
    // And because snapSn is normally smaller than metaPage_.sn, snapSn should
    // also be smaller than sn
    // There may be several situations where metaPage_.sn <= snap.sn
    // Scenario 1: DataStore restarts to restore historical logs,
    // metaPage_.sn==snap.sn may appear
    // There was a request to generate a snapshot file before the restart,
    // but it restarted before the metapage was updated
    // After restarting, the previous operation is played back, and the sn of
    // this operation is equal to the sn of the current chunk
    // Scenario 2: The follower downloads a snapshot of the raft through the
    // leader when restoring the raft
    // During the download process, the chunk on the leader is also taking a
    // snapshot of the chunk, and the follower will do log recovery after
    // downloading
    // Since follower downloads the chunk file first, and then downloads the
    // snapshot file, so at this time metaPage_.sn<=snap.sn
    if (sn != metaPage_.sn || metaPage_.sn <= snapshots_->getCurrentSn()) {
        LOG(WARNING) << "May be a log repaly opt after an unexpected restart."
                     << "Request sn: " << sn
                     << ", chunk sn: " << metaPage_.sn
                     << ", snapshot sn: " << snapshots_->getCurrentSn();
        return false;
    }
    return true;
}

CSErrorCode CSChunkFile::updateMetaPage(ChunkFileMetaPage* metaPage) {
    std::unique_ptr<char[]> buf(new char[pageSize_]);
    memset(buf.get(), 0, pageSize_);
    metaPage->encode(buf.get());
    int rc = writeMetaPage(buf.get());
    if (rc < 0) {
        LOG(ERROR) << "Update metapage failed."
                   << "ChunkID: " << chunkId_
                   << ",chunk sn: " << metaPage_.sn;
        return CSErrorCode::InternalError;
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::loadMetaPage() {
    std::unique_ptr<char[]> buf(new char[pageSize_]);
    memset(buf.get(), 0, pageSize_);
    int rc = readMetaPage(buf.get());
    if (rc < 0) {
        LOG(ERROR) << "Error occured when reading metaPage_."
                   << " filepath = " << path();
        return CSErrorCode::InternalError;
    }
    return metaPage_.decode(buf.get());
}

CSErrorCode CSChunkFile::copy2Snapshot(off_t offset, size_t length) {
    // Get the uncopied area in the snapshot file
    uint32_t pageBeginIndex = offset / pageSize_;
    uint32_t pageEndIndex = (offset + length - 1) / pageSize_;
    std::vector<BitRange> uncopiedRange;
    CSSnapshot* snapshot_ = snapshots_->getCurrentSnapshot();
    std::shared_ptr<const Bitmap> snapBitmap = snapshot_->GetPageStatus();
    snapBitmap->Divide(pageBeginIndex,
                       pageEndIndex,
                       &uncopiedRange,
                       nullptr);

    CSErrorCode errorCode = CSErrorCode::Success;
    off_t copyOff;
    size_t copySize;
    // Read the uncopied area from the chunk file
    // and write it to the snapshot file
    for (auto& range : uncopiedRange) {
        copyOff = range.beginIndex * pageSize_;
        copySize = (range.endIndex - range.beginIndex + 1) * pageSize_;
        std::shared_ptr<char> buf(new char[copySize],
                                  std::default_delete<char[]>());
        int rc = readData(buf.get(),
                          copyOff,
                          copySize);
        if (rc < 0) {
            LOG(ERROR) << "Read from chunk file failed."
                       << "ChunkID: " << chunkId_
                       << ",chunk sn: " << metaPage_.sn;
            return CSErrorCode::InternalError;
        }
        errorCode = snapshot_->Write(buf.get(), copyOff, copySize);
        if (errorCode != CSErrorCode::Success) {
            LOG(ERROR) << "Write to snapshot failed."
                       << "ChunkID: " << chunkId_
                       << ",chunk sn: " << metaPage_.sn
                       << ",snapshot sn: " << snapshot_->GetSn();
            return errorCode;
        }
    }
    // If the snapshot file has been written,
    // you need to call Flush to persist the metapage
    if (uncopiedRange.size() > 0) {
        errorCode = snapshot_->Flush();
        if (errorCode != CSErrorCode::Success) {
            LOG(ERROR) << "Flush snapshot metapage failed."
                        << "ChunkID: " << chunkId_
                        << ",chunk sn: " << metaPage_.sn
                        << ",snapshot sn: " << snapshot_->GetSn();
            return errorCode;
        }
    }
    return CSErrorCode::Success;
}

CSErrorCode CSChunkFile::flush() {
    ChunkFileMetaPage tempMeta = metaPage_;
    bool needUpdateMeta = dirtyPages_.size() > 0;
    bool clearClone = false;
    for (auto pageIndex : dirtyPages_) {
        tempMeta.bitmap->Set(pageIndex);
    }
    if (isCloneChunk_) {
        // If all pages have been written, mark the Chunk as a non-clone chunk
        if (tempMeta.bitmap->NextClearBit(0) == Bitmap::NO_POS) {
            tempMeta.location = "";
            tempMeta.bitmap = nullptr;
            needUpdateMeta = true;
            clearClone = true;
        }
    }
    if (needUpdateMeta) {
        CSErrorCode errorCode = updateMetaPage(&tempMeta);
        if (errorCode != CSErrorCode::Success) {
            LOG(ERROR) << "Update metapage failed."
                        << "ChunkID: " << chunkId_
                        << ",chunk sn: " << metaPage_.sn;
            return errorCode;
        }
        metaPage_.bitmap = tempMeta.bitmap;
        metaPage_.location = tempMeta.location;
        dirtyPages_.clear();
        if (clearClone) {
            if (metric_ != nullptr) {
                metric_->cloneChunkCount << -1;
            }
            isCloneChunk_ = false;
        }
    }
    return CSErrorCode::Success;
}

}  // namespace chunkserver
}  // namespace curve

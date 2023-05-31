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
 * Created Date: Friday March 29th 2019
 * Author: dxiang@corp.netease.com
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <glog/logging.h>

#include "include/chunkserver/chunkserver_common.h"
#include "src/chunkserver/datastore/chunkserver_datastore.h"
#include "src/chunkserver/datastore/chunkserver_chunkfile.h"
#include "src/chunkserver/datastore/chunkserver_snapshot.h"

#include "test/chunkserver/datastore/mock_file_pool.h"
#include "test/fs/mock_local_filesystem.h"

using curve::fs::LocalFileSystem;
using curve::fs::MockLocalFileSystem;
using curve::common::Bitmap;

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::NotNull;
using ::testing::SetArgPointee;
using ::testing::SetArrayArgument;

using std::shared_ptr;
using std::make_shared;
namespace curve {

namespace chunkserver {

const uint64_t cid = 1;
const uint64_t csn = 0;
const char baseDir[] = "/home/dxiang/copyset/data";
const ChunkSizeType CHUNK_SIZE = 16 * 1024 * 1024;
const PageSizeType PAGE_SIZE = 4096;
const uint32_t kLocationLimit = 3000;
const bool cSyncOrNot = true;
const uint64_t cloneno = 0;

const char chunk1Path[] = "/home/dxiang/copyset/data/chunk_1";
const char chunk1Clone1Path[] = "/home/dxiang/copyset/data/chunk_1_clone_1";
const char chunk1Snap1Path[] = "/home/dxiang/copyset/data/chunk_1_snap_2";
const char chunk1Clone1Snap1Path[] = "/home/dxiang/copyset/data/chunk_1_clone_1_snap_2";

class CSChunkFile_test : public testing::Test {

public:
    void SetUp() override {
        lfs_ = std::make_shared<MockLocalFileSystem>();
        fpool_ = std::make_shared<MockFilePool>(lfs_);
        
        DataStoreOptions options;
        options.baseDir = baseDir;
        options.chunkSize = CHUNK_SIZE;
        options.pageSize = PAGE_SIZE;
        options.locationLimit = kLocationLimit;
        options.enableOdsyncWhenOpenChunkFile = cSyncOrNot;
        dataStore_ = std::make_shared<CSDataStore>(lfs_, fpool_, options);
    }

    void TearDown() override { }

    static void SetUpTestSuite () {
        std::cout<<"run before first case..."<<std::endl;
    }

    static void TearDownTestSuite () {
        std::cout<<"run after last case..."<<std::endl;
    }

    void FakeEnv() {
        EXPECT_CALL(*lfs_, DirExists(baseDir))
                .WillRepeatedly(Return(true));
        
        EXPECT_CALL(*lfs_, FileExists(chunk1Path))
                .WillOnce(Return(false))
                .WillRepeatedly(Return(true));
        
        EXPECT_CALL(*lfs_, Open(chunk1Path, _))
                .WillRepeatedly(Return(1));
        
        struct stat fileInfo;
        fileInfo.st_size = CHUNK_SIZE + PAGE_SIZE;
        EXPECT_CALL(*lfs_, Fstat(_, _))
                .WillRepeatedly(DoAll(SetArgPointee<1>(fileInfo), Return(0)));
        
        EXPECT_CALL(*lfs_, Close(1))
                .WillRepeatedly(Return(0));
        
        // fake read chunk1 metapage
        FakeEncodeChunk(chunk1MetaPage, 0, 2);
        EXPECT_CALL(*lfs_, Read(1, NotNull(), 0, PAGE_SIZE))
                .WillRepeatedly(DoAll(
                        SetArrayArgument<1>(chunk1MetaPage,
                        chunk1MetaPage + PAGE_SIZE),
                        Return(PAGE_SIZE)));
    }

    inline void FakeEncodeChunk(char* buf,
                            SequenceNum correctedSn,
                            SequenceNum sn,
                            shared_ptr<Bitmap> bitmap = nullptr,
                            const std::string& location = "") {
        ChunkFileMetaPage metaPage;
        metaPage.version = FORMAT_VERSION;
        metaPage.sn = sn;
        metaPage.correctedSn = correctedSn;
        metaPage.bitmap = bitmap;
        metaPage.location = location;
        metaPage.encode(buf);
    }
    

    inline void FakeEncodeSnapshot(char* buf,
                                SequenceNum sn) {
        uint32_t bits = CHUNK_SIZE / PAGE_SIZE;
        ChunkFileMetaPage metaPage;
        metaPage.version = FORMAT_VERSION;
        metaPage.sn = sn;
        metaPage.bitmap = std::make_shared<Bitmap>(bits);
        metaPage.encode(buf);
    }


protected:
    std::shared_ptr<MockLocalFileSystem> lfs_;
    std::shared_ptr<MockFilePool> fpool_;
    std::shared_ptr<CSDataStore> dataStore_;
    char chunk1MetaPage[PAGE_SIZE];
    char chunk1Clone1MetaPage[PAGE_SIZE];
    char chunk1Snap1MetaPage[PAGE_SIZE];
    char chunk1Clone1Snap1MetaPage[PAGE_SIZE];
    int fdMock;
};

TEST_F(CSChunkFile_test, CloneOpen) {
    
    CSChunkFilePtr chunkFile = nullptr;
    ChunkOptions options;

    FakeEnv();

    options.id = cid;
    options.sn = csn;
    options.baseDir = baseDir;
    options.chunkSize = CHUNK_SIZE;
    options.location = std::string();
    options.pageSize = PAGE_SIZE;
    options.metric = std::make_shared<DataStoreMetric>();
    options.enableOdsyncWhenOpenChunkFile = cSyncOrNot;
    CSErrorCode errorCode = dataStore_->CreateChunkFile(options, &chunkFile);
    EXPECT_EQ (CSErrorCode::Success, errorCode);
}

TEST_F(CSChunkFile_test, CloneOpenWithChunk) {
    CSChunkFilePtr chunkFile = nullptr;
    CSChunkFilePtr cloneChunkFile = nullptr;
    uint64_t cloneNo = 1;
    ChunkOptions options;

    //Build the Environment
    EXPECT_CALL(*lfs_, DirExists(baseDir))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, FileExists(chunk1Path))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, FileExists(chunk1Clone1Path))
        .WillOnce(Return(false))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, Open(chunk1Path, _))
        .WillRepeatedly(Return(1));
    EXPECT_CALL(*lfs_, Open(chunk1Clone1Path, _))
        .WillRepeatedly(Return(2));
    struct stat fileInfo;
    fileInfo.st_size = CHUNK_SIZE + PAGE_SIZE;
    EXPECT_CALL(*lfs_, Fstat(_, _))
        .WillRepeatedly(DoAll(SetArgPointee<1>(fileInfo), Return(0)));
    FakeEncodeChunk(chunk1Clone1MetaPage, 0, 0);
    EXPECT_CALL(*lfs_, Read(2, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1Clone1MetaPage,
                chunk1Clone1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));

    FakeEncodeChunk(chunk1MetaPage, 0, 0);
    EXPECT_CALL(*lfs_, Read(1, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1MetaPage,
                chunk1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));

    EXPECT_CALL(*lfs_, Close(1))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(*lfs_, Close(2))
        .WillRepeatedly(Return(0));

    options.id = cid;
    options.sn = csn;
    options.baseDir = baseDir;
    options.chunkSize = CHUNK_SIZE;
    options.location = std::string();
    options.pageSize = PAGE_SIZE;
    options.metric = std::make_shared<DataStoreMetric>();
    options.enableOdsyncWhenOpenChunkFile = cSyncOrNot;

    CSErrorCode errorCode = CSErrorCode::Success;
    errorCode = dataStore_->CreateChunkFile(options, &chunkFile);
    EXPECT_EQ (CSErrorCode::Success, errorCode);

    errorCode = dataStore_->CreateChunkFile(options, chunkFile, &cloneChunkFile, cloneNo);
    EXPECT_EQ (CSErrorCode::Success, errorCode);

}



TEST_F(CSChunkFile_test, InitializeTest) {

    //Build the Environment
    EXPECT_CALL(*lfs_, DirExists(baseDir))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, FileExists(chunk1Path))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, FileExists(chunk1Clone1Path))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(*lfs_, Open(chunk1Path, _))
        .WillRepeatedly(Return(1));
    EXPECT_CALL(*lfs_, Open(chunk1Clone1Path, _))
        .WillRepeatedly(Return(2));
    EXPECT_CALL(*lfs_, Open(chunk1Snap1Path, _))
        .WillRepeatedly(Return(3));
    EXPECT_CALL(*lfs_, Open(chunk1Clone1Snap1Path, _))
        .WillRepeatedly(Return(4));
    struct stat fileInfo;
    fileInfo.st_size = CHUNK_SIZE + PAGE_SIZE;
    EXPECT_CALL(*lfs_, Fstat(_, _))
        .WillRepeatedly(DoAll(SetArgPointee<1>(fileInfo), Return(0)));
    FakeEncodeChunk(chunk1Clone1MetaPage, 0, 0);
    EXPECT_CALL(*lfs_, Read(2, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1Clone1MetaPage,
                chunk1Clone1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));

    FakeEncodeChunk(chunk1MetaPage, 0, 0);
    EXPECT_CALL(*lfs_, Read(1, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1MetaPage,
                chunk1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));
   
    FakeEncodeSnapshot(chunk1Snap1MetaPage, 2);
    EXPECT_CALL(*lfs_, Read(3, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1Snap1MetaPage,
                chunk1Snap1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));

    FakeEncodeSnapshot(chunk1Clone1Snap1MetaPage, 2);
    EXPECT_CALL(*lfs_, Read(4, NotNull(), 0, PAGE_SIZE))
        .WillRepeatedly(DoAll(
                SetArrayArgument<1>(chunk1Clone1Snap1MetaPage,
                chunk1Clone1Snap1MetaPage + PAGE_SIZE),
                Return(PAGE_SIZE)));

    EXPECT_CALL(*lfs_, Close(1))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(*lfs_, Close(2))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(*lfs_, Close(3))
        .WillRepeatedly(Return(0));
    EXPECT_CALL(*lfs_, Close(4))
        .WillRepeatedly(Return(0));

    vector<string> fileNames;
    fileNames.push_back ("chunk_1");
    fileNames.push_back ("chunk_1_clone_1");
    fileNames.push_back ("chunk_1_snap_2");
    fileNames.push_back ("chunk_1_clone_1_snap_2");
    EXPECT_CALL(*lfs_, List(baseDir, NotNull()))
        .WillRepeatedly(DoAll(SetArgPointee<1>(fileNames),
        Return(0)));

    dataStore_->DebugPrint();
    EXPECT_TRUE(dataStore_->Initialize());
}


}

}
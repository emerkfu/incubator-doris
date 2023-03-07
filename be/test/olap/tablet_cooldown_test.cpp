// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include <memory>

#include "common/config.h"
#include "common/status.h"
#include "exec/tablet_info.h"
#include "gen_cpp/internal_service.pb.h"
#include "io/fs/file_writer.h"
#include "io/fs/local_file_system.h"
#include "io/fs/local_file_writer.h"
#include "io/fs/remote_file_system.h"
#include "io/fs/s3_file_system.h"
#include "olap/delta_writer.h"
#include "olap/rowset/beta_rowset.h"
#include "olap/storage_engine.h"
#include "olap/storage_policy.h"
#include "olap/tablet.h"
#include "runtime/descriptor_helper.h"
#include "util/file_utils.h"
#include "util/s3_util.h"

namespace doris {

static StorageEngine* k_engine = nullptr;

static const std::string kTestDir = "ut_dir/tablet_cooldown_test";
static constexpr int64_t kResourceId = 10000;
static constexpr int64_t kStoragePolicyId = 10002;
static constexpr int64_t kTabletId = 10005;
static constexpr int64_t kTabletId2 = 10006;
static constexpr int64_t kReplicaId = 10009;
static constexpr int32_t kSchemaHash = 270068377;
static constexpr int64_t kReplicaId2 = 10010;
static constexpr int32_t kSchemaHash2 = 270068381;

static constexpr int32_t kTxnId = 20003;
static constexpr int32_t kPartitionId = 30003;
static constexpr int32_t kTxnId2 = 40003;
static constexpr int32_t kPartitionId2 = 50003;

using io::Path;

static io::FileSystemSPtr s_fs;

static std::string get_remote_path(const Path& path) {
    return fmt::format("{}/remote/{}", config::storage_root_path, path.string());
}

class FileWriterMock : public io::FileWriter {
public:
    FileWriterMock(Path path) : io::FileWriter(std::move(path)) {
        io::global_local_filesystem()->create_file(get_remote_path(_path), &_local_file_writer);
    }

    ~FileWriterMock() override = default;

    Status close() override { return _local_file_writer->close(); }

    Status abort() override { return _local_file_writer->abort(); }

    Status append(const Slice& data) override { return _local_file_writer->append(data); }

    Status appendv(const Slice* data, size_t data_cnt) override {
        return _local_file_writer->appendv(data, data_cnt);
    }

    Status write_at(size_t offset, const Slice& data) override {
        return _local_file_writer->write_at(offset, data);
    }

    Status finalize() override { return _local_file_writer->finalize(); }

    size_t bytes_appended() const override { return _local_file_writer->bytes_appended(); }

    io::FileSystemSPtr fs() const override { return s_fs; }

private:
    std::unique_ptr<io::FileWriter> _local_file_writer;
};

class RemoteFileSystemMock : public io::RemoteFileSystem {
public:
    RemoteFileSystemMock(Path root_path, std::string&& id, io::FileSystemType type)
            : RemoteFileSystem(std::move(root_path), std::move(id), type) {
        _local_fs = io::LocalFileSystem::create(get_remote_path(_root_path));
    }
    ~RemoteFileSystemMock() override = default;

    Status create_file(const Path& path, io::FileWriterPtr* writer) override {
        Path fs_path = path;
        *writer = std::make_unique<FileWriterMock>(fs_path);
        return Status::OK();
    }

    Status open_file(const Path& path, io::FileReaderSPtr* reader, IOContext* io_ctx) override {
        return _local_fs->open_file(get_remote_path(path), reader, io_ctx);
    }

    Status delete_file(const Path& path) override {
        return _local_fs->delete_file(get_remote_path(path));
    }

    Status create_directory(const Path& path) override {
        return _local_fs->create_directory(get_remote_path(path));
    }

    Status delete_directory(const Path& path) override {
        return _local_fs->delete_directory(get_remote_path(path));
    }

    Status link_file(const Path& src, const Path& dest) override {
        return _local_fs->link_file(get_remote_path(src), get_remote_path(dest));
    }

    Status exists(const Path& path, bool* res) const override {
        return _local_fs->exists(get_remote_path(path), res);
    }

    Status file_size(const Path& path, size_t* file_size) const override {
        return _local_fs->file_size(get_remote_path(path), file_size);
    }

    Status list(const Path& path, std::vector<Path>* files) override {
        std::vector<Path> local_paths;
        RETURN_IF_ERROR(_local_fs->list(get_remote_path(path), &local_paths));
        for (Path path : local_paths) {
            files->emplace_back(path.string().substr(config::storage_root_path.size() + 1));
        }
        return Status::OK();
    }

    Status upload(const Path& local_path, const Path& dest_path) override {
        return _local_fs->link_file(local_path.string(), get_remote_path(dest_path));
    }

    Status batch_upload(const std::vector<Path>& local_paths,
                        const std::vector<Path>& dest_paths) override {
        for (int i = 0; i < local_paths.size(); ++i) {
            RETURN_IF_ERROR(upload(local_paths[i], dest_paths[i]));
        }
        return Status::OK();
    }

    Status batch_delete(const std::vector<Path>& paths) override {
        for (int i = 0; i < paths.size(); ++i) {
            RETURN_IF_ERROR(delete_file(paths[i]));
        }
        return Status::OK();
    }

    Status connect() override { return Status::OK(); }

private:
    std::shared_ptr<io::LocalFileSystem> _local_fs;
};

class TabletCooldownTest : public testing::Test {
public:
    static void SetUpTestSuite() {
        s_fs.reset(new RemoteFileSystemMock("test_path", std::to_string(kResourceId),
                                            io::FileSystemType::S3));
        StorageResource resource = {s_fs, 1};
        put_storage_resource(kResourceId, resource);
        auto storage_policy = std::make_shared<StoragePolicy>();
        storage_policy->name = "TabletCooldownTest";
        storage_policy->version = 1;
        storage_policy->resource_id = kResourceId;
        put_storage_policy(kStoragePolicyId, storage_policy);

        constexpr uint32_t MAX_PATH_LEN = 1024;
        char buffer[MAX_PATH_LEN];
        EXPECT_NE(getcwd(buffer, MAX_PATH_LEN), nullptr);
        config::storage_root_path = std::string(buffer) + "/" + kTestDir;
        config::min_file_descriptor_number = 1000;

        FileUtils::remove_all(config::storage_root_path);
        FileUtils::create_dir(config::storage_root_path);
        FileUtils::create_dir(get_remote_path(fmt::format("data/{}", kTabletId)));
        FileUtils::create_dir(get_remote_path(fmt::format("data/{}", kTabletId2)));

        std::vector<StorePath> paths {{config::storage_root_path, -1}};

        EngineOptions options;
        options.store_paths = paths;
        doris::StorageEngine::open(options, &k_engine);
    }

    static void TearDownTestSuite() {
        if (k_engine != nullptr) {
            k_engine->stop();
            delete k_engine;
            k_engine = nullptr;
        }
    }
};

static void create_tablet_request_with_sequence_col(int64_t tablet_id, int32_t schema_hash,
                                                    TCreateTabletReq* request) {
    request->tablet_id = tablet_id;
    request->__set_version(1);
    request->tablet_schema.schema_hash = schema_hash;
    request->tablet_schema.short_key_column_count = 2;
    request->tablet_schema.keys_type = TKeysType::UNIQUE_KEYS;
    request->tablet_schema.storage_type = TStorageType::COLUMN;
    request->tablet_schema.__set_sequence_col_idx(2);
    request->__set_storage_format(TStorageFormat::V2);

    TColumn k1;
    k1.column_name = "k1";
    k1.__set_is_key(true);
    k1.column_type.type = TPrimitiveType::TINYINT;
    request->tablet_schema.columns.push_back(k1);

    TColumn k2;
    k2.column_name = "k2";
    k2.__set_is_key(true);
    k2.column_type.type = TPrimitiveType::SMALLINT;
    request->tablet_schema.columns.push_back(k2);

    TColumn sequence_col;
    sequence_col.column_name = SEQUENCE_COL;
    sequence_col.__set_is_key(false);
    sequence_col.column_type.type = TPrimitiveType::INT;
    sequence_col.__set_aggregation_type(TAggregationType::REPLACE);
    request->tablet_schema.columns.push_back(sequence_col);

    TColumn v1;
    v1.column_name = "v1";
    v1.__set_is_key(false);
    v1.column_type.type = TPrimitiveType::DATETIME;
    v1.__set_aggregation_type(TAggregationType::REPLACE);
    request->tablet_schema.columns.push_back(v1);
}

static TDescriptorTable create_descriptor_tablet_with_sequence_col() {
    TDescriptorTableBuilder desc_tbl_builder;
    TTupleDescriptorBuilder tuple_builder;

    tuple_builder.add_slot(
            TSlotDescriptorBuilder().type(TYPE_TINYINT).column_name("k1").column_pos(0).build());
    tuple_builder.add_slot(
            TSlotDescriptorBuilder().type(TYPE_SMALLINT).column_name("k2").column_pos(1).build());
    tuple_builder.add_slot(TSlotDescriptorBuilder()
                                   .type(TYPE_INT)
                                   .column_name(SEQUENCE_COL)
                                   .column_pos(2)
                                   .build());
    tuple_builder.add_slot(
            TSlotDescriptorBuilder().type(TYPE_DATETIME).column_name("v1").column_pos(3).build());
    tuple_builder.build(&desc_tbl_builder);

    return desc_tbl_builder.desc_tbl();
}

void createTablet(StorageEngine* engine, TabletSharedPtr* tablet, int64_t replica_id,
                  int32_t schema_hash, int64_t tablet_id, int64_t txn_id, int64_t partition_id) {
    // create tablet
    TCreateTabletReq request;
    create_tablet_request_with_sequence_col(tablet_id, schema_hash, &request);
    request.__set_replica_id(replica_id);
    Status st = engine->create_tablet(request);
    ASSERT_EQ(Status::OK(), st);

    TDescriptorTable tdesc_tbl = create_descriptor_tablet_with_sequence_col();
    ObjectPool obj_pool;
    DescriptorTbl* desc_tbl = nullptr;
    DescriptorTbl::create(&obj_pool, tdesc_tbl, &desc_tbl);
    TupleDescriptor* tuple_desc = desc_tbl->get_tuple_descriptor(0);
    OlapTableSchemaParam param;

    // write data
    PUniqueId load_id;
    load_id.set_hi(0);
    load_id.set_lo(0);

    WriteRequest write_req = {tablet_id, schema_hash, WriteType::LOAD,        txn_id, partition_id,
                              load_id,   tuple_desc,  &(tuple_desc->slots()), false,  &param};
    DeltaWriter* delta_writer = nullptr;
    DeltaWriter::open(&write_req, &delta_writer);
    ASSERT_NE(delta_writer, nullptr);

    vectorized::Block block;
    for (const auto& slot_desc : tuple_desc->slots()) {
        block.insert(vectorized::ColumnWithTypeAndName(slot_desc->get_empty_mutable_column(),
                                                       slot_desc->get_data_type_ptr(),
                                                       slot_desc->col_name()));
    }

    auto columns = block.mutate_columns();
    {
        int8_t c1 = 123;
        columns[0]->insert_data((const char*)&c1, sizeof(c1));

        int16_t c2 = 456;
        columns[1]->insert_data((const char*)&c2, sizeof(c2));

        int32_t c3 = 1;
        columns[2]->insert_data((const char*)&c3, sizeof(c2));

        DateTimeValue c4;
        c4.from_date_str("2020-07-16 19:39:43", 19);
        int64_t c4_int = c4.to_int64();
        columns[3]->insert_data((const char*)&c4_int, sizeof(c4));

        st = delta_writer->write(&block, {0});
        ASSERT_EQ(Status::OK(), st);
    }

    st = delta_writer->close();
    ASSERT_EQ(Status::OK(), st);
    st = delta_writer->close_wait(PSlaveTabletNodes(), false);
    ASSERT_EQ(Status::OK(), st);
    delete delta_writer;

    // publish version success
    *tablet = engine->tablet_manager()->get_tablet(write_req.tablet_id, write_req.schema_hash);
    OlapMeta* meta = (*tablet)->data_dir()->get_meta();
    Version version;
    version.first = (*tablet)->rowset_with_max_version()->end_version() + 1;
    version.second = (*tablet)->rowset_with_max_version()->end_version() + 1;
    std::map<TabletInfo, RowsetSharedPtr> tablet_related_rs;
    engine->txn_manager()->get_txn_related_tablets(write_req.txn_id, write_req.partition_id,
                                                   &tablet_related_rs);
    for (auto& tablet_rs : tablet_related_rs) {
        RowsetSharedPtr rowset = tablet_rs.second;
        st = engine->txn_manager()->publish_txn(meta, write_req.partition_id, write_req.txn_id,
                                                (*tablet)->tablet_id(), (*tablet)->schema_hash(),
                                                (*tablet)->tablet_uid(), version);
        ASSERT_EQ(Status::OK(), st);
        st = (*tablet)->add_inc_rowset(rowset);
        ASSERT_EQ(Status::OK(), st);
    }
    EXPECT_EQ(1, (*tablet)->num_rows());
}

TEST_F(TabletCooldownTest, normal) {
    TabletSharedPtr tablet1;
    TabletSharedPtr tablet2;
    createTablet(k_engine, &tablet1, kReplicaId, kSchemaHash, kTabletId, kTxnId, kPartitionId);
    createTablet(k_engine, &tablet2, kReplicaId2, kSchemaHash2, kTabletId2, kTxnId2, kPartitionId2);
    // test cooldown
    tablet1->set_storage_policy_id(kStoragePolicyId);
    Status st = tablet1->cooldown(); // rowset [0-1]
    ASSERT_NE(Status::OK(), st);
    tablet1->update_cooldown_conf(1, kReplicaId);
    // cooldown for upload node
    st = tablet1->cooldown(); // rowset [0-1]
    ASSERT_EQ(Status::OK(), st);
    st = tablet1->cooldown(); // rowset [2-2]
    ASSERT_EQ(Status::OK(), st);
    auto rs = tablet1->get_rowset_by_version({2, 2});
    ASSERT_FALSE(rs->is_local());

    // test read
    ASSERT_EQ(Status::OK(), st);
    std::vector<segment_v2::SegmentSharedPtr> segments;
    st = std::static_pointer_cast<BetaRowset>(rs)->load_segments(&segments);
    ASSERT_EQ(Status::OK(), st);
    ASSERT_EQ(segments.size(), 1);

    st = io::global_local_filesystem()->link_file(
            get_remote_path(fmt::format("data/{}/{}.{}.meta", kTabletId, kReplicaId, 1)),
            get_remote_path(fmt::format("data/{}/{}.{}.meta", kTabletId2, kReplicaId, 2)));
    ASSERT_EQ(Status::OK(), st);
    // follow cooldown
    tablet2->set_storage_policy_id(kStoragePolicyId);
    tablet2->update_cooldown_conf(1, 111111111);
    st = tablet2->cooldown(); // rowset [0-1]
    ASSERT_NE(Status::OK(), st);
    tablet2->update_cooldown_conf(1, kReplicaId);
    st = tablet2->cooldown(); // rowset [0-1]
    ASSERT_NE(Status::OK(), st);
    tablet2->update_cooldown_conf(2, kReplicaId);
    st = tablet2->cooldown(); // rowset [0-1]
    ASSERT_EQ(Status::OK(), st);
    auto rs2 = tablet2->get_rowset_by_version({2, 2});
    ASSERT_FALSE(rs2->is_local());

    // test read tablet2
    ASSERT_EQ(Status::OK(), st);
    std::vector<segment_v2::SegmentSharedPtr> segments2;
    st = std::static_pointer_cast<BetaRowset>(rs2)->load_segments(&segments2);
    ASSERT_EQ(Status::OK(), st);
    ASSERT_EQ(segments2.size(), 1);
}

} // namespace doris

#include "tsuba/RDG.h"

#include <cassert>
#include <exception>
#include <fstream>
#include <memory>
#include <regex>
#include <unordered_set>

#include <arrow/filesystem/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <parquet/platform.h>
#include <parquet/properties.h>

#include "AddTables.h"
#include "GlobalState.h"
#include "RDGCore.h"
#include "RDGHandleImpl.h"
#include "galois/Backtrace.h"
#include "galois/JSON.h"
#include "galois/Logging.h"
#include "galois/Result.h"
#include "galois/Uri.h"
#include "tsuba/Errors.h"
#include "tsuba/FaultTest.h"
#include "tsuba/file.h"
#include "tsuba/tsuba.h"

using json = nlohmann::json;

namespace {

// special partition property names
const char* kMirrorNodesPropName = "mirror_nodes";
const char* kMasterNodesPropName = "master_nodes";
const char* kLocalToTGlobalPropName = "local_to_global_vector";

std::shared_ptr<parquet::WriterProperties>
StandardWriterProperties() {
  // int64 timestamps with nanosecond resolution requires Parquet version 2.0.
  // In Arrow to Parquet version 1.0, nanosecond timestamps will get truncated
  // to milliseconds.
  return parquet::WriterProperties::Builder()
      .version(parquet::ParquetVersion::PARQUET_2_0)
      ->data_page_version(parquet::ParquetDataPageVersion::V2)
      ->build();
}

std::shared_ptr<parquet::ArrowWriterProperties>
StandardArrowProperties() {
  return parquet::ArrowWriterProperties::Builder().build();
}

/// Store the arrow array as a table in a unique file, return
/// the final name of that file
galois::Result<std::string>
DoStoreArrowArrayAtName(
    const std::shared_ptr<arrow::ChunkedArray>& array, const galois::Uri& dir,
    const std::string& name, tsuba::WriteGroup* desc) {
  galois::Uri next_path = dir.RandFile(name);

  // Metadata paths should relative to dir
  std::shared_ptr<arrow::Table> column = arrow::Table::Make(
      arrow::schema({arrow::field(name, array->type())}), {array});

  auto ff = std::make_shared<tsuba::FileFrame>();
  if (auto res = ff->Init(); !res) {
    return res.error();
  }

  auto write_result = parquet::arrow::WriteTable(
      *column, arrow::default_memory_pool(), ff,
      std::numeric_limits<int64_t>::max(), StandardWriterProperties(),
      StandardArrowProperties());

  if (!write_result.ok()) {
    GALOIS_LOG_DEBUG("arrow error: {}", write_result);
    return tsuba::ErrorCode::ArrowError;
  }

  ff->Bind(next_path.string());
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  desc->StartStore(std::move(ff));
  return next_path.BaseName();
}

galois::Result<std::string>
StoreArrowArrayAtName(
    const std::shared_ptr<arrow::ChunkedArray>& array, const galois::Uri& dir,
    const std::string& name, tsuba::WriteGroup* desc) {
  try {
    return DoStoreArrowArrayAtName(array, dir, name, desc);
  } catch (const std::exception& exp) {
    GALOIS_LOG_DEBUG("arrow exception: {}", exp.what());
    return tsuba::ErrorCode::ArrowError;
  }
}

std::string
MirrorPropName(unsigned i) {
  return std::string(kMirrorNodesPropName) + "_" + std::to_string(i);
}

std::string
MasterPropName(unsigned i) {
  return std::string(kMasterNodesPropName) + "_" + std::to_string(i);
}

galois::Result<std::vector<tsuba::PropStorageInfo>>
WriteTable(
    const arrow::Table& table,
    const std::vector<tsuba::PropStorageInfo>& properties,
    const galois::Uri& dir, tsuba::WriteGroup* desc) {
  const auto& schema = table.schema();

  std::vector<std::string> next_paths;
  for (size_t i = 0, n = properties.size(); i < n; ++i) {
    if (!properties[i].persist || !properties[i].path.empty()) {
      continue;
    }
    auto name = properties[i].name.empty() ? schema->field(i)->name()
                                           : properties[i].name;
    auto name_res = StoreArrowArrayAtName(table.column(i), dir, name, desc);
    if (!name_res) {
      return name_res.error();
    }
    next_paths.emplace_back(name_res.value());
  }
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);

  if (next_paths.empty()) {
    return properties;
  }

  std::vector<tsuba::PropStorageInfo> next_properties = properties;
  auto it = next_paths.begin();
  for (auto& v : next_properties) {
    if (v.persist && v.path.empty()) {
      v.path = *it++;
    }
  }

  return next_properties;
}

galois::Result<void>
CommitRDG(
    tsuba::RDGHandle handle, uint32_t policy_id, bool transposed,
    const tsuba::RDGLineage& lineage, std::unique_ptr<tsuba::WriteGroup> desc) {
  galois::CommBackend* comm = tsuba::Comm();
  tsuba::RDGMeta new_meta = handle.impl_->rdg_meta().NextVersion(
      comm->Num, policy_id, transposed, lineage);

  // wait for all the work we queued to finish
  TSUBA_PTP(tsuba::internal::FaultSensitivity::High);
  if (auto res = desc->Finish(); !res) {
    GALOIS_LOG_ERROR("at least one async write failed: {}", res.error());
    return res.error();
  }
  TSUBA_PTP(tsuba::internal::FaultSensitivity::High);
  comm->Barrier();

  // NS handles MPI coordination
  if (auto res = tsuba::NS()->Update(
          handle.impl_->rdg_meta().dir(), handle.impl_->rdg_meta().version(),
          new_meta);
      !res) {
    GALOIS_LOG_ERROR(
        "unable to update rdg at {}: {}", handle.impl_->rdg_meta().dir(),
        res.error());
  }

  TSUBA_PTP(tsuba::internal::FaultSensitivity::High);
  galois::Result<void> ret = tsuba::OneHostOnly([&]() -> galois::Result<void> {
    TSUBA_PTP(tsuba::internal::FaultSensitivity::High);

    std::string curr_s = new_meta.ToJsonString();
    auto res = tsuba::FileStore(
        tsuba::RDGMeta::FileName(
            handle.impl_->rdg_meta().dir(), new_meta.version())
            .string(),
        reinterpret_cast<const uint8_t*>(curr_s.data()), curr_s.size());
    if (!res) {
      GALOIS_LOG_ERROR(
          "CommitRDG future failed {}: {}",
          tsuba::RDGMeta::FileName(
              handle.impl_->rdg_meta().dir(), new_meta.version()),
          res.error());
      return res.error();
    }
    return galois::ResultSuccess();
  });
  if (ret) {
    handle.impl_->set_rdg_meta(std::move(new_meta));
  }
  return ret;
}

}  // namespace

galois::Result<void>
tsuba::RDG::AddPartitionMetadataArray(
    const std::shared_ptr<arrow::Table>& table) {
  auto field = table->schema()->field(0);
  const std::string& name = field->name();
  std::shared_ptr<arrow::ChunkedArray> col = table->column(0);
  if (name.find(kMirrorNodesPropName) == 0) {
    AddMirrorNodes(std::move(col));
  } else if (name.find(kMasterNodesPropName) == 0) {
    AddMasterNodes(std::move(col));
  } else if (name == kLocalToTGlobalPropName) {
    set_local_to_global_vector(std::move(col));
  } else {
    return tsuba::ErrorCode::InvalidArgument;
  }
  return galois::ResultSuccess();
}

void
tsuba::RDG::AddLineage(const std::string& command_line) {
  lineage_.AddCommandLine(command_line);
}

tsuba::RDGFile::~RDGFile() {
  auto result = Close(handle_);
  if (!result) {
    GALOIS_LOG_ERROR("closing RDGFile: {}", result.error());
  }
}

galois::Result<std::vector<tsuba::PropStorageInfo>>
tsuba::RDG::WritePartArrays(const galois::Uri& dir, tsuba::WriteGroup* desc) {
  std::vector<tsuba::PropStorageInfo> next_properties;

  GALOIS_LOG_DEBUG(
      "WritePartArrays master sz: {} mirros sz: {} l2g sz: {}",
      master_nodes_.size(), mirror_nodes_.size(),
      local_to_global_vector_ == nullptr ? 0
                                         : local_to_global_vector_->length());

  for (unsigned i = 0; i < mirror_nodes_.size(); ++i) {
    auto name = MirrorPropName(i);
    auto mirr_res = StoreArrowArrayAtName(mirror_nodes_[i], dir, name, desc);
    if (!mirr_res) {
      return mirr_res.error();
    }
    next_properties.emplace_back(tsuba::PropStorageInfo{
        .name = name,
        .path = std::move(mirr_res.value()),
        .persist = true,
    });
  }

  for (unsigned i = 0; i < master_nodes_.size(); ++i) {
    auto name = MasterPropName(i);
    auto mast_res = StoreArrowArrayAtName(master_nodes_[i], dir, name, desc);
    if (!mast_res) {
      return mast_res.error();
    }
    next_properties.emplace_back(tsuba::PropStorageInfo{
        .name = name,
        .path = std::move(mast_res.value()),
        .persist = true,
    });
  }

  if (local_to_global_vector_ != nullptr) {
    auto l2g_res = StoreArrowArrayAtName(
        local_to_global_vector_, dir, kLocalToTGlobalPropName, desc);
    if (!l2g_res) {
      return l2g_res.error();
    }
    next_properties.emplace_back(tsuba::PropStorageInfo{
        .name = kLocalToTGlobalPropName,
        .path = std::move(l2g_res.value()),
        .persist = true,
    });
  }

  return next_properties;
}

galois::Result<void>
tsuba::RDG::DoStore(
    RDGHandle handle, const std::string& command_line,
    std::unique_ptr<WriteGroup> write_group) {
  if (core_->part_header().topology_path().empty()) {
    // No topology file; create one
    galois::Uri t_path = handle.impl_->rdg_meta().dir().RandFile("topology");

    TSUBA_PTP(internal::FaultSensitivity::Normal);

    // depends on `topology_file_storage_` outliving writes
    write_group->StartStore(
        t_path.string(), core_->topology_file_storage().ptr<uint8_t>(),
        core_->topology_file_storage().size());
    TSUBA_PTP(internal::FaultSensitivity::Normal);
    core_->part_header().set_topology_path(t_path.BaseName());
  }

  auto node_write_result = WriteTable(
      *core_->node_table(), core_->part_header().node_prop_info_list(),
      handle.impl_->rdg_meta().dir(), write_group.get());
  if (!node_write_result) {
    GALOIS_LOG_DEBUG("failed to write node properties");
    return node_write_result.error();
  }

  // update node properties with newly written locations
  core_->part_header().set_node_prop_info_list(
      std::move(node_write_result.value()));

  auto edge_write_result = WriteTable(
      *core_->edge_table(), core_->part_header().edge_prop_info_list(),
      handle.impl_->rdg_meta().dir(), write_group.get());
  if (!edge_write_result) {
    GALOIS_LOG_DEBUG("failed to write edge properties");
    return edge_write_result.error();
  }

  // update edge properties with newly written locations
  core_->part_header().set_edge_prop_info_list(
      std::move(edge_write_result.value()));

  auto part_write_result =
      WritePartArrays(handle.impl_->rdg_meta().dir(), write_group.get());

  if (!part_write_result) {
    GALOIS_LOG_DEBUG("failed: WritePartMetadata for part_prop_info_list");
    return part_write_result.error();
  }
  core_->part_header().set_part_properties(
      std::move(part_write_result.value()));

  if (auto write_result = core_->part_header().Write(handle, write_group.get());
      !write_result) {
    GALOIS_LOG_DEBUG("error: metadata write");
    return write_result.error();
  }

  // Update lineage and commit
  lineage_.AddCommandLine(command_line);
  if (auto res = CommitRDG(
          handle, core_->part_header().metadata().policy_id_,
          core_->part_header().metadata().transposed_, lineage_,
          std::move(write_group));
      !res) {
    return res.error();
  }
  return galois::ResultSuccess();
}

galois::Result<void>
tsuba::RDG::DoMake(const galois::Uri& metadata_dir) {
  auto node_result = AddTables(
      metadata_dir, core_->part_header().node_prop_info_list(),
      [rdg = this](const std::shared_ptr<arrow::Table>& table) {
        return rdg->core_->AddNodeProperties(table);
      });
  if (!node_result) {
    return node_result.error();
  }

  auto edge_result = AddTables(
      metadata_dir, core_->part_header().edge_prop_info_list(),
      [rdg = this](const std::shared_ptr<arrow::Table>& table) {
        return rdg->core_->AddEdgeProperties(table);
      });
  if (!edge_result) {
    return edge_result.error();
  }

  const std::vector<PropStorageInfo>& part_prop_info_list =
      core_->part_header().part_prop_info_list();
  if (!part_prop_info_list.empty()) {
    auto part_result = AddTables(
        metadata_dir, part_prop_info_list,
        [rdg = this](const std::shared_ptr<arrow::Table>& table) {
          return rdg->AddPartitionMetadataArray(table);
        });
    if (!part_result) {
      return edge_result.error();
    }
  }

  galois::Uri t_path = metadata_dir.Join(core_->part_header().topology_path());
  if (auto res = core_->topology_file_storage().Bind(t_path.string(), true);
      !res) {
    return res.error();
  }

  rdg_dir_ = metadata_dir;
  return galois::ResultSuccess();
}

galois::Result<tsuba::RDG>
tsuba::RDG::Make(
    const RDGMeta& meta, const std::vector<std::string>* node_props,
    const std::vector<std::string>* edge_props) {
  if (!meta.IsEmptyRDG() && meta.num_hosts() != Comm()->Num) {
    GALOIS_LOG_ERROR(
        "number of hosts for partitioned graph does not current number of "
        "hosts: {} vs {}",
        meta.num_hosts(), Comm()->Num);
    // Query depends on being able to load a graph this way
    /*
    if (meta.num_hosts() == 1) {
      // TODO(thunt) eliminate this special case after query is updated not
      // to depend on this behavior
      return PartitionFileName(meta.dir(), 0, version());
    }
    */
    return ErrorCode::InvalidArgument;
  }

  galois::Uri partition_path = meta.PartitionFileName(Comm()->ID);

  auto part_header_res = RDGPartHeader::Make(partition_path);
  if (!part_header_res) {
    GALOIS_LOG_DEBUG(
        "failed: ReadMetaData (path: {}): {}", partition_path,
        part_header_res.error());
    return part_header_res.error();
  }

  RDG rdg(std::make_unique<RDGCore>(std::move(part_header_res.value())));

  if (auto res = rdg.core_->part_header().PrunePropsTo(node_props, edge_props);
      !res) {
    return res.error();
  }

  if (auto res = rdg.DoMake(meta.dir()); !res) {
    return res.error();
  }

  return RDG(std::move(rdg));
}

galois::Result<void>
tsuba::RDG::Validate() const {
  if (auto res = core_->part_header().Validate(); !res) {
    return res.error();
  }
  return galois::ResultSuccess();
}

bool
tsuba::RDG::Equals(const RDG& other) const {
  return core_->Equals(*other.core_);
}

galois::Result<tsuba::RDG>
tsuba::RDG::Make(
    RDGHandle handle, const std::vector<std::string>* node_props,
    const std::vector<std::string>* edge_props) {
  if (!handle.impl_->AllowsRead()) {
    GALOIS_LOG_DEBUG("failed: handle does not allow full read");
    return ErrorCode::InvalidArgument;
  }
  return RDG::Make(handle.impl_->rdg_meta(), node_props, edge_props);
}

galois::Result<void>
tsuba::RDG::Store(
    RDGHandle handle, const std::string& command_line,
    std::unique_ptr<FileFrame> ff) {
  if (!handle.impl_->AllowsWrite()) {
    GALOIS_LOG_DEBUG("failed: handle does not allow write");
    return ErrorCode::InvalidArgument;
  }
  // We trust the partitioner to give us a valid graph, but we
  // report our assumptions
  GALOIS_LOG_DEBUG(
      "RDG::Store meta.num_hosts: {} meta.policy_id: {} num_hosts: {} "
      "policy_id: {}",
      handle.impl_->rdg_meta().num_hosts(),
      handle.impl_->rdg_meta().policy_id(), tsuba::Comm()->Num,
      core_->part_header().metadata().policy_id_);
  if (handle.impl_->rdg_meta().dir() != rdg_dir_) {
    core_->part_header().UnbindFromStorage();
  }

  auto desc_res = WriteGroup::Make();
  if (!desc_res) {
    return desc_res.error();
  }
  // All write buffers must outlive desc
  std::unique_ptr<WriteGroup> desc = std::move(desc_res.value());

  if (ff) {
    galois::Uri t_path = handle.impl_->rdg_meta().dir().RandFile("topology");

    ff->Bind(t_path.string());
    TSUBA_PTP(internal::FaultSensitivity::Normal);
    desc->StartStore(std::move(ff));
    TSUBA_PTP(internal::FaultSensitivity::Normal);
    core_->part_header().set_topology_path(t_path.BaseName());
  }

  return DoStore(handle, command_line, std::move(desc));
}

galois::Result<void>
tsuba::RDG::AddNodeProperties(const std::shared_ptr<arrow::Table>& table) {
  if (auto res = core_->AddNodeProperties(table); !res) {
    return res.error();
  }

  const auto& schema = table->schema();
  for (int i = 0, end = table->num_columns(); i < end; ++i) {
    core_->part_header().AppendNodePropStorageInfo(tsuba::PropStorageInfo{
        .name = schema->field(i)->name(),
        .path = "",
    });
  }

  assert(
      static_cast<size_t>(core_->node_table()->num_columns()) ==
      core_->part_header().node_prop_info_list().size());

  return galois::ResultSuccess();
}

galois::Result<void>
tsuba::RDG::AddEdgeProperties(const std::shared_ptr<arrow::Table>& table) {
  if (auto res = core_->AddEdgeProperties(table); !res) {
    return res.error();
  }

  const auto& schema = table->schema();
  for (int i = 0, end = table->num_columns(); i < end; ++i) {
    core_->part_header().AppendEdgePropStorageInfo(tsuba::PropStorageInfo{
        .name = schema->field(i)->name(),
        .path = "",
    });
  }

  assert(
      static_cast<size_t>(core_->edge_table()->num_columns()) ==
      core_->part_header().edge_prop_info_list().size());

  return galois::ResultSuccess();
}

galois::Result<void>
tsuba::RDG::RemoveNodeProperty(uint32_t i) {
  return core_->RemoveNodeProperty(i);
}

galois::Result<void>
tsuba::RDG::RemoveEdgeProperty(uint32_t i) {
  return core_->RemoveEdgeProperty(i);
}

void
tsuba::RDG::MarkAllPropertiesPersistent() {
  core_->part_header().MarkAllPropertiesPersistent();
}

galois::Result<void>
tsuba::RDG::MarkNodePropertiesPersistent(
    const std::vector<std::string>& persist_node_props) {
  return core_->part_header().MarkNodePropertiesPersistent(persist_node_props);
}

galois::Result<void>
tsuba::RDG::MarkEdgePropertiesPersistent(
    const std::vector<std::string>& persist_edge_props) {
  return core_->part_header().MarkEdgePropertiesPersistent(persist_edge_props);
}

const tsuba::PartitionMetadata&
tsuba::RDG::part_metadata() const {
  return core_->part_header().metadata();
}

void
tsuba::RDG::set_part_metadata(const tsuba::PartitionMetadata& metadata) {
  core_->part_header().set_metadata(metadata);
}

const std::shared_ptr<arrow::Table>&
tsuba::RDG::node_table() const {
  return core_->node_table();
}

const std::shared_ptr<arrow::Table>&
tsuba::RDG::edge_table() const {
  return core_->edge_table();
}

const tsuba::FileView&
tsuba::RDG::topology_file_storage() const {
  return core_->topology_file_storage();
}

galois::Result<void>
tsuba::RDG::UnbindTopologyFileStorage() {
  return core_->topology_file_storage().Unbind();
}

tsuba::RDG::RDG(std::unique_ptr<RDGCore>&& core) : core_(std::move(core)) {}

tsuba::RDG::RDG() : core_(std::make_unique<RDGCore>()) {}

tsuba::RDG::~RDG() = default;
tsuba::RDG::RDG(tsuba::RDG&& other) noexcept = default;
tsuba::RDG& tsuba::RDG::operator=(tsuba::RDG&& other) noexcept = default;

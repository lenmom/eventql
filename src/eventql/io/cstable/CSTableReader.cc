/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "eventql/eventql.h"
#include <eventql/io/cstable/CSTableReader.h>
#include <eventql/io/cstable/io/PageReader.h>
#include <eventql/io/cstable/columns/v1/BooleanColumnReader.h>
#include <eventql/io/cstable/columns/v1/BitPackedIntColumnReader.h>
#include <eventql/io/cstable/columns/v1/UInt32ColumnReader.h>
#include <eventql/io/cstable/columns/v1/UInt64ColumnReader.h>
#include <eventql/io/cstable/columns/v1/LEB128ColumnReader.h>
#include <eventql/io/cstable/columns/v1/DoubleColumnReader.h>
#include <eventql/io/cstable/columns/v1/StringColumnReader.h>
#include <eventql/io/cstable/columns/column_reader_uint.h>
#include <eventql/io/cstable/columns/page_reader_uint64.h>
#include <eventql/util/io/file.h>
#include <eventql/util/io/mmappedfile.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cstable {

static RefPtr<v1::ColumnReader> openColumnV1(
    const ColumnConfig& c,
    RefPtr<MmappedFile> mmap) {
  auto csize = c.body_size;
  auto cdata = mmap->structAt<void>(c.body_offset);
  auto rmax = c.rlevel_max;
  auto dmax = c.dlevel_max;

  switch (c.storage_type) {
    case ColumnEncoding::BOOLEAN_BITPACKED:
      return new v1::BooleanColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::UINT32_BITPACKED:
      return new v1::BitPackedIntColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::UINT32_PLAIN:
      return new v1::UInt32ColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::UINT64_PLAIN:
      return new v1::UInt64ColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::UINT64_LEB128:
      return new v1::LEB128ColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::FLOAT_IEEE754:
      return new v1::DoubleColumnReader(rmax, dmax, cdata, csize);
    case ColumnEncoding::STRING_PLAIN:
      return new v1::StringColumnReader(rmax, dmax, cdata, csize);
    default:
      RAISEF(
          kRuntimeError,
          "unsupported column type: $0",
          (uint32_t) c.storage_type);
  }
}

static RefPtr<ColumnReader> openColumnV2(
    const ColumnConfig& c,
    PageManager* page_mgr) {
  ScopedPtr<UnsignedIntPageReader> rlevel_reader;
  ScopedPtr<UnsignedIntPageReader> dlevel_reader;

  if (c.rlevel_max > 0) {
    PageIndexKey rlevel_idx_key {
      .column_id = c.column_id,
      .entry_type = PageIndexEntryType::RLEVEL
    };

    rlevel_reader = mkScoped(new UInt64PageReader(page_mgr));
  }

  switch (c.logical_type) {
    case ColumnType::UNSIGNED_INT:
      return new UnsignedIntColumnReader(
          c,
          std::move(rlevel_reader),
          std::move(dlevel_reader),
          page_mgr);
    default:
      RAISEF(
          kRuntimeError,
          "unsupported column type: $0",
          (uint32_t) c.storage_type);
  }
}

RefPtr<CSTableReader> CSTableReader::openFile(const String& filename) {
  auto file = File::openFile(filename, File::O_READ);
  auto file_is = FileInputStream::fromFileDescriptor(file.fd());

  BinaryFormatVersion version;
  FileHeader header;
  MetaBlock metablock;
  Option<PageRef> free_index;
  readHeader(
      &version,
      &header,
      &metablock,
      &free_index,
      file_is.get());

  switch (version) {
    case BinaryFormatVersion::v0_1_0: {
      auto mmap = mkRef(new MmappedFile(std::move(file)));

      Vector<RefPtr<ColumnReader>> column_readers;
      for (const auto& col : header.columns) {
        auto reader = openColumnV1(col, mmap.get());
        reader->storeMmap(mmap.get());
        column_readers.emplace_back(reader.get());
      }

      return new CSTableReader(
          version,
          ScopedPtr<PageManager>(nullptr),
          header.columns,
          column_readers,
          header.num_rows,
          -1);
    }

    case BinaryFormatVersion::v0_2_0: {
      auto page_mgr = mkScoped(new PageManager(file.fd(), 0, {}));

      Vector<RefPtr<ColumnReader>> column_readers;
      for (const auto& col : header.columns) {
        column_readers.emplace_back(openColumnV2(col, page_mgr.get()));
      }

      return new CSTableReader(
          version,
          std::move(page_mgr),
          header.columns,
          column_readers,
          metablock.num_rows,
          file.releaseFD());
    }
  }
}

CSTableReader::CSTableReader(
    BinaryFormatVersion version,
    ScopedPtr<PageManager> page_mgr,
    Vector<ColumnConfig> columns,
    Vector<RefPtr<ColumnReader>> column_readers,
    uint64_t num_rows,
    int fd) :
    version_(version),
    page_mgr_(std::move(page_mgr)),
    columns_(columns),
    num_rows_(num_rows),
    fd_(fd) {
  RCHECK(column_readers.size() == columns.size(), "illegal column list");

  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns_[i].column_id > 0) {
      column_readers_by_id_.emplace(columns_[i].column_id, column_readers[i]);
    }

    column_readers_by_name_.emplace(columns_[i].column_name, column_readers[i]);
  }
}

CSTableReader::~CSTableReader() {
  if (fd_ > 0) {
    close(fd_);
  }
}

RefPtr<ColumnReader> CSTableReader::getColumnReader(
    const String& column_name) {
  auto col = column_readers_by_name_.find(column_name);
  if (col == column_readers_by_name_.end()) {
    RAISEF(kNotFoundError, "column not found: $0", column_name);
  }

  return col->second;
}

ColumnEncoding CSTableReader::getColumnEncoding(const String& column_name) {
  auto col = getColumnReader(column_name);
  return col->encoding();
}

ColumnType CSTableReader::getColumnType(const String& column_name) {
  auto col = getColumnReader(column_name);
  return col->type();
}

const Vector<ColumnConfig>& CSTableReader::columns() const {
  return columns_;
}

bool CSTableReader::hasColumn(const String& column_name) const {
  auto col = column_readers_by_name_.find(column_name);
  return col != column_readers_by_name_.end();
}

size_t CSTableReader::numRecords() const {
  return num_rows_;
}


} // namespace cstable



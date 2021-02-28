/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <exception>

#include <unistd.h>

#include <boost/program_options.hpp>

#include <folly/Conv.h>
#include <folly/String.h>

#include <archive.h>
#include <archive_entry.h>

#include "dwarfs/filesystem_v2.h"
#include "dwarfs/fstypes.h"
#include "dwarfs/logger.h"
#include "dwarfs/mmap.h"
#include "dwarfs/options.h"
#include "dwarfs/util.h"
#include "dwarfs/version.h"

namespace po = boost::program_options;

using namespace dwarfs;

namespace {

int dwarfsextract(int argc, char** argv) {
  std::string filesystem, output, format, cache_size_str, log_level;
  size_t num_workers;

  // clang-format off
  po::options_description opts("Command line options");
  opts.add_options()
    ("input,i",
        po::value<std::string>(&filesystem),
        "input filesystem file")
    ("output,o",
        po::value<std::string>(&output),
        "output file or directory")
    ("format,f",
        po::value<std::string>(&format),
        "output format")
    ("num-workers,n",
        po::value<size_t>(&num_workers)->default_value(1),
        "number of worker threads")
    ("cache-size,s",
        po::value<std::string>(&cache_size_str)->default_value("256m"),
        "block cache size")
    ("log-level,l",
        po::value<std::string>(&log_level)->default_value("warn"),
        "log level (error, warn, info, debug, trace)")
    ("help,h",
        "output help message and exit");
  // clang-format on

  po::variables_map vm;

  try {
    po::store(po::parse_command_line(argc, argv, opts), vm);
    po::notify(vm);
  } catch (po::error const& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }

  if (vm.count("help") or !vm.count("input")) {
    std::cerr << "dwarfsextract (" << PRJ_GIT_ID << ")\n\n"
              << opts << std::endl;
    return 0;
  }

  try {
    stream_logger lgr(std::cerr, logger::parse_level(log_level));
    filesystem_options fsopts;

    fsopts.block_cache.max_bytes = parse_size_with_unit(cache_size_str);
    fsopts.block_cache.num_workers = num_workers;
    fsopts.metadata.enable_nlink = true;

    dwarfs::filesystem_v2 fs(lgr, std::make_shared<dwarfs::mmap>(filesystem),
                             fsopts);

    log_proxy<debug_logger_policy> log_(lgr);
    struct ::archive* a;

    auto check_result = [&](int res) {
      switch (res) {
      case ARCHIVE_OK:
        break;
      case ARCHIVE_WARN:
        LOG_WARN << std::string(archive_error_string(a));
        break;
      case ARCHIVE_RETRY:
      case ARCHIVE_FATAL:
        DWARFS_THROW(runtime_error, std::string(archive_error_string(a)));
      }
    };

    if (format.empty()) {
      if (!output.empty()) {
        if (::chdir(output.c_str()) != 0) {
          DWARFS_THROW(runtime_error,
                       output + ": " + std::string(strerror(errno)));
        }
      }

      a = ::archive_write_disk_new();

      check_result(::archive_write_disk_set_options(
          a,
          ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME));
    } else {
      a = ::archive_write_new();

      check_result(::archive_write_set_format_by_name(a, format.c_str()));
      check_result(::archive_write_open_filename(
          a, vm.count("output") && !output.empty() && output != "-"
                 ? output.c_str()
                 : nullptr));
    }

    auto lr = ::archive_entry_linkresolver_new();

    ::archive_entry_linkresolver_set_strategy(lr, ::archive_format(a));

    ::archive_entry* spare = nullptr;

    auto do_archive = [&](::archive_entry* ae, entry_view entry) {
      check_result(::archive_write_header(a, ae));
      if (auto size = ::archive_entry_size(ae); size > 0) {
        int fh = fs.open(entry);
        iovec_read_buf irb;
        fs.readv(fh, irb, size, 0);
        for (auto const& iov : irb.buf) {
          check_result(::archive_write_data(a, iov.iov_base, iov.iov_len));
        }
      }
    };

    fs.walk([&](auto entry, auto parent) {
      if (entry.inode() == 0) {
        return;
      }

      auto ae = ::archive_entry_new();
      struct ::stat stbuf;

      if (fs.getattr(entry, &stbuf) != 0) {
        DWARFS_THROW(runtime_error, "getattr() failed");
      }

      std::string path = parent.path();
      if (!path.empty()) {
        path += '/';
      }
      path += entry.name();

      ::archive_entry_set_pathname(ae, path.c_str());
      ::archive_entry_copy_stat(ae, &stbuf);

      if (S_ISLNK(entry.mode())) {
        std::string link;
        if (fs.readlink(entry, &link) != 0) {
          LOG_ERROR << "readlink() failed";
        }
        ::archive_entry_set_symlink(ae, link.c_str());
      }

      ::archive_entry_linkify(lr, &ae, &spare);

      if (ae) {
        do_archive(ae, entry);
        ::archive_entry_free(ae);
      }

      if (spare) {
        auto ev = fs.find(::archive_entry_ino(spare));
        if (!ev) {
          LOG_ERROR << "find() failed";
        }
        LOG_DEBUG << "archiving spare " << ::archive_entry_pathname(spare);
        do_archive(spare, *ev);
        ::archive_entry_free(spare);
      }
    });

    // As we're visiting *all* hardlinks, we should never see any deferred
    // entries.
    ::archive_entry* ae = nullptr;
    ::archive_entry_linkify(lr, &ae, &spare);
    if (ae) {
      DWARFS_THROW(runtime_error, "unexpected deferred entry");
    }

    ::archive_entry_linkresolver_free(lr);
    check_result(::archive_write_free(a));
  } catch (runtime_error const& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  } catch (system_error const& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}

} // namespace

int main(int argc, char** argv) {
  return dwarfs::safe_main([&] { return dwarfsextract(argc, argv); });
}

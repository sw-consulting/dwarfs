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

#include <cassert>
#include <cerrno>

#ifdef _WIN32
#include <folly/portability/Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "dwarfs/error.h"
#include "dwarfs/mmap.h"

namespace dwarfs {

namespace {

uint64_t get_page_size() {
#ifdef _WIN32
  ::SYSTEM_INFO info;
  ::GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return ::sysconf(_SC_PAGESIZE);
#endif
}

} // namespace

std::error_code
mmap::lock(file_off_t offset [[maybe_unused]], size_t size [[maybe_unused]]) {
  std::error_code ec;

#ifdef _WIN32
  if (::VirtualLock(mf_.data() + offset, size) == 0) {
    ec.assign(::GetLastError(), std::system_category());
  }
#else
  if (::mlock(mf_.const_data() + offset, size) != 0) {
    ec.assign(errno, std::generic_category());
  }
#endif

  return ec;
}

std::error_code mmap::release(file_off_t offset [[maybe_unused]],
                              size_t size [[maybe_unused]]) {
  std::error_code ec;

  auto misalign = offset % page_size_;

  offset -= misalign;
  size += misalign;
  size -= size % page_size_;

#ifdef _WIN32
  if (::VirtualFree(mf_.data() + offset, size, MEM_DECOMMIT) == 0) {
    ec.assign(::GetLastError(), std::system_category());
  }
#else
  if (::madvise(mf_.data() + offset, size, MADV_DONTNEED) != 0) {
    ec.assign(errno, std::generic_category());
  }
#endif

  return ec;
}

std::error_code mmap::release_until(file_off_t offset [[maybe_unused]]) {
  std::error_code ec;

  offset -= offset % page_size_;

#ifdef _WIN32
  if (::VirtualFree(mf_.data(), offset, MEM_DECOMMIT) == 0) {
    ec.assign(::GetLastError(), std::system_category());
  }
#else
  if (::madvise(mf_.data(), offset, MADV_DONTNEED) != 0) {
    ec.assign(errno, std::generic_category());
  }
#endif

  return ec;
}

void const* mmap::addr() const { return mf_.const_data(); }

size_t mmap::size() const { return mf_.size(); }

mmap::mmap(std::string const& path)
    : mf_(path, boost::iostreams::mapped_file::readonly)
    , page_size_(get_page_size()) {
  assert(mf_.is_open());
}

mmap::mmap(std::filesystem::path const& path)
    : mmap(path.string()) {}

mmap::mmap(std::string const& path, size_t size)
    : mf_(path, boost::iostreams::mapped_file::readonly, size)
    , page_size_(get_page_size()) {
  assert(mf_.is_open());
}

mmap::mmap(std::filesystem::path const& path, size_t size)
    : mmap(path.string(), size) {}

} // namespace dwarfs

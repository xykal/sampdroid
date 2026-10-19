// Read-only descriptor-backed ByteSource for Android Storage Access Framework.
#include "gta_file_source.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace samp::gta::img {

FileDescriptorByteSource::FileDescriptorByteSource(int ownedFileDescriptor, uint64_t fileSize)
    : fileDescriptor_(ownedFileDescriptor), fileSize_(fileSize) {}

FileDescriptorByteSource::~FileDescriptorByteSource() {
  if (fileDescriptor_ >= 0) {
    ::close(fileDescriptor_);
    fileDescriptor_ = -1;
  }
}

std::unique_ptr<FileDescriptorByteSource> FileDescriptorByteSource::duplicateFrom(
    int fileDescriptor, std::string& error) {
  error.clear();
  if (fileDescriptor < 0) {
    error = "file descriptor data GTA tidak valid";
    return nullptr;
  }

  const int duplicate = ::dup(fileDescriptor);
  if (duplicate < 0) {
    error = "gagal menduplikasi file descriptor data GTA";
    return nullptr;
  }
  const int descriptorFlags = ::fcntl(duplicate, F_GETFD);
  if (descriptorFlags >= 0) {
    (void)::fcntl(duplicate, F_SETFD, descriptorFlags | FD_CLOEXEC);
  }

  struct stat metadata {};
  // Some document providers expose an anonymous/proxy fd rather than a named
  // regular file. `pread` only needs an accurate length and seekability, not a
  // filesystem path or a particular inode type.
  if (::fstat(duplicate, &metadata) != 0 || metadata.st_size < 0 ||
      ::lseek(duplicate, 0, SEEK_CUR) < 0) {
    ::close(duplicate);
    error = "data GTA harus berupa file seekable read-only";
    return nullptr;
  }
  return std::unique_ptr<FileDescriptorByteSource>(
      new FileDescriptorByteSource(duplicate, static_cast<uint64_t>(metadata.st_size)));
}

uint64_t FileDescriptorByteSource::size() const {
  return fileSize_;
}

bool FileDescriptorByteSource::readAt(uint64_t offset, uint8_t* destination, size_t count) const {
  if (fileDescriptor_ < 0 || (count != 0 && destination == nullptr) || offset > fileSize_ ||
      static_cast<uint64_t>(count) > fileSize_ - offset) {
    return false;
  }
  if (count == 0) return true;

  constexpr uint64_t kMaxOffset = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
  if (offset > kMaxOffset) return false;
  size_t consumed = 0;
  while (consumed < count) {
    const uint64_t nextOffset = offset + static_cast<uint64_t>(consumed);
    if (nextOffset > kMaxOffset) return false;
    const size_t remaining = count - consumed;
    const size_t chunk = std::min(remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    const ssize_t read = ::pread(fileDescriptor_, destination + consumed, chunk,
                                 static_cast<off_t>(nextOffset));
    if (read < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (read == 0) return false;
    consumed += static_cast<size_t>(read);
  }
  return true;
}

}  // namespace samp::gta::img

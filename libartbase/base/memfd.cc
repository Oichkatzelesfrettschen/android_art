/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "memfd.h"

#include <errno.h>
#include <stdio.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__BIONIC__)
#include <linux/memfd.h>  // To access memfd flags.
#endif

#include <android-base/logging.h>
#if defined(ART_TARGET_ANDROID)
#include <android-base/properties.h>
#endif
#include <android-base/unique_fd.h>

#include "macros.h"


// When building for linux host, glibc in prebuilts does not include memfd_create system call
// number. As a temporary testing measure, we add the definition here.
#if defined(__linux__) && !defined(__NR_memfd_create)
#if defined(__x86_64__)
#define __NR_memfd_create 319
#elif defined(__i386__)
#define __NR_memfd_create 356
#endif  // defined(__i386__)
#endif  // defined(__linux__) && !defined(__NR_memfd_create)

namespace art {

#if defined(__NR_memfd_create)

static bool KernelSupportsMemfdCreate() {
  // Check kernel version supports memfd_create(). Some older kernels segfault executing
  // memfd_create() rather than returning ENOSYS (b/116769556).
  static constexpr int kRequiredMajor = 3;
  static constexpr int kRequiredMinor = 17;
  struct utsname uts;
  int major, minor;
  if (uname(&uts) != 0 ||
      strcmp(uts.sysname, "Linux") != 0 ||
      sscanf(uts.release, "%d.%d", &major, &minor) != 2) {
    return false;
  }
  if (major > kRequiredMajor || (major == kRequiredMajor && minor >= kRequiredMinor)) {
    return true;
  }
#if defined(ART_TARGET_ANDROID)
  // An older kernel that carries the memfd_create backport extends its syscall table past
  // __NR_memfd_create, so the call returns instead of trapping; the device declares the
  // backport through this property.
  return ::android::base::GetBoolProperty("ro.kernel.memfd_create.supported", false);
#else
  return false;
#endif
}

int memfd_create(const char* name, unsigned int flags) {
  static const bool kernel_supports_memfd_create = KernelSupportsMemfdCreate();
  if (!kernel_supports_memfd_create) {
    errno = ENOSYS;
    return -1;
  }

  return syscall(__NR_memfd_create, name, flags);
}

#else  // __NR_memfd_create

int memfd_create(const char* name ATTRIBUTE_UNUSED, unsigned int flags ATTRIBUTE_UNUSED) {
  errno = ENOSYS;
  return -1;
}

#endif  // __NR_memfd_create

// This is a wrapper that will attempt to simulate memfd_create if normal running fails.
int memfd_create_compat(const char* name, unsigned int flags) {
  int res = memfd_create(name, flags);
  if (res >= 0) {
    return res;
  }
#if !defined(_WIN32)
  // Try to create an anonymous file with tmpfile that we can use instead.
  if (flags == 0) {
    FILE* file = tmpfile();
    if (file != nullptr) {
      // We want the normal 'dup' semantics since memfd_create without any flags isn't CLOEXEC.
      // Unfortunately on some android targets we will compiler error if we use dup directly and so
      // need to use fcntl.
      int nfd = fcntl(fileno(file), F_DUPFD, /*lowest allowed fd*/ 0);
      fclose(file);
      return nfd;
    }
  }
#endif
  return res;
}

#if defined(__BIONIC__)

static bool IsSealFutureWriteSupportedInternal() {
  android::base::unique_fd fd(art::memfd_create("test_android_memfd", MFD_ALLOW_SEALING));
  if (fd == -1) {
    LOG(INFO) << "memfd_create failed: " << strerror(errno) << ", no memfd support.";
    return false;
  }

  if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == -1) {
    LOG(INFO) << "fcntl(F_ADD_SEALS) failed: " << strerror(errno) << ", no memfd support.";
    return false;
  }

  LOG(INFO) << "Using memfd for future sealing";
  return true;
}

bool IsSealFutureWriteSupported() {
  static bool is_seal_future_write_supported = IsSealFutureWriteSupportedInternal();
  return is_seal_future_write_supported;
}

#else

bool IsSealFutureWriteSupported() {
  return false;
}

#endif

}  // namespace art

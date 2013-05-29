//===-- sanitizer_platform_limits_posix.h ---------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of Sanitizer common code.
//
// Sizes and layouts of platform-specific POSIX data structures.
//===----------------------------------------------------------------------===//

#ifndef SANITIZER_PLATFORM_LIMITS_POSIX_H
#define SANITIZER_PLATFORM_LIMITS_POSIX_H

#include "sanitizer_platform.h"

namespace __sanitizer {
  extern unsigned struct_utsname_sz;
  extern unsigned struct_stat_sz;
  extern unsigned struct_stat64_sz;
  extern unsigned struct_rusage_sz;
  extern unsigned struct_tm_sz;
  extern unsigned struct_passwd_sz;
  extern unsigned struct_group_sz;
  extern unsigned struct_sigaction_sz;
  extern unsigned siginfo_t_sz;
  extern unsigned struct_itimerval_sz;
  extern unsigned pthread_t_sz;

#if !SANITIZER_ANDROID
  extern unsigned ucontext_t_sz;
#endif // !SANITIZER_ANDROID

#if SANITIZER_LINUX
  extern unsigned struct_rlimit_sz;
  extern unsigned struct_dirent_sz;
  extern unsigned struct_statfs_sz;
  extern unsigned struct_epoll_event_sz;
  extern unsigned struct_timespec_sz;
#endif // SANITIZER_LINUX

#if SANITIZER_LINUX && !SANITIZER_ANDROID
  extern unsigned struct_dirent64_sz;
  extern unsigned struct_rlimit64_sz;
  extern unsigned struct_statfs64_sz;
#endif // SANITIZER_LINUX && !SANITIZER_ANDROID

  struct __sanitizer_iovec {
    void  *iov_base;
    uptr iov_len;
  };

#if SANITIZER_ANDROID || SANITIZER_MAC
  struct __sanitizer_msghdr {
    void *msg_name;
    unsigned msg_namelen;
    struct __sanitizer_iovec *msg_iov;
    unsigned msg_iovlen;
    void *msg_control;
    unsigned msg_controllen;
    int msg_flags;
  };
  struct __sanitizer_cmsghdr {
    unsigned cmsg_len;
    int cmsg_level;
    int cmsg_type;
  };
#else
  struct __sanitizer_msghdr {
    void *msg_name;
    unsigned msg_namelen;
    struct __sanitizer_iovec *msg_iov;
    uptr msg_iovlen;
    void *msg_control;
    uptr msg_controllen;
    int msg_flags;
  };
  struct __sanitizer_cmsghdr {
    uptr cmsg_len;
    int cmsg_level;
    int cmsg_type;
  };
#endif

  // This thing depends on the platform. We are only interested in the upper
  // limit. Verified with a compiler assert in .cc.
  const int pthread_attr_t_max_sz = 128;
  union __sanitizer_pthread_attr_t {
    char size[pthread_attr_t_max_sz]; // NOLINT
    void *align;
  };

  uptr __sanitizer_get_sigaction_sa_sigaction(void *act);
  void __sanitizer_set_sigaction_sa_sigaction(void *act, uptr cb);
  bool __sanitizer_get_sigaction_sa_siginfo(void *act);

  const unsigned struct_sigaction_max_sz = 256;
  union __sanitizer_sigaction {
    char size[struct_sigaction_max_sz]; // NOLINT
  };

  extern uptr sig_ign;
  extern uptr sig_dfl;

  uptr __sanitizer_in_addr_sz(int af);

#if SANITIZER_LINUX
  struct __sanitizer_dl_phdr_info {
    uptr dlpi_addr;
    const char *dlpi_name;
    const void *dlpi_phdr;
    short dlpi_phnum;
  };
#endif

  struct __sanitizer_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
#if SANITIZER_ANDROID || SANITIZER_MAC
    unsigned ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
#else // LINUX
    uptr ai_addrlen;
    void *ai_addr;
    char *ai_canonname;
#endif
    struct __sanitizer_addrinfo *ai_next;
  };

  struct __sanitizer_hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
  };

}  // namespace __sanitizer

#endif

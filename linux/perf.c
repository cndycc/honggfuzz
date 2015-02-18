/*
 *
 * honggfuzz - architecture dependent code (LINUX/PERF)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"

#include <fcntl.h>
#include <inttypes.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "linux/perf.h"
#include "log.h"

/*
 * 4MB + 1page
 */
#define _HF_PERF_MMAP_SZ ((size_t)getpagesize() + (1024 * 1024 * 4))

static __thread void *perfMmap = NULL;
static __thread bool perfOverflow = false;

static uint64_t arch_perfParseMmap(void)
{
//    struct perf_event_mmap_page *pem = (struct perf_event_mmap_page *)perfMmap;
    struct perf_event_header *peh = (struct perf_event_header *)(perfMmap + getpagesize());

    for (;;) {
        if (peh->type != PERF_RECORD_SAMPLE) {
            LOGMSG(l_ERROR, "perf_event_header->type != PERF_RECORD_SAMPLE (%" PRId32 ")",
                   peh->type);
            break;
        }
        if (peh->misc != PERF_RECORD_MISC_USER) {
            LOGMSG(l_ERROR, "perf_event_header->misc != PERF_RECORD_MISC_USER (%" PRId16 ")",
                   peh->misc);
            break;
        }

        uint64_t bnr = *(uint64_t *) (perfMmap + getpagesize() + sizeof(struct perf_event_header));
        struct perf_branch_entry *lbr =
            (struct perf_branch_entry *)(perfMmap + getpagesize() +
                                         sizeof(struct perf_event_header) + bnr);

        LOGMSG(l_INFO, "PEHSIZE: %llu BNR: %llu", peh->size, bnr);

        for (uint64_t i = 0; i < bnr; i++) {
            LOGMSG(l_ERROR, "FROM: %llx TO: %llx", lbr[i].from, lbr[i].to);
        }

        int *i = (int *)0x44444444;
        *i = 66;

        peh = (struct perf_event_header *)((uint8_t *) peh + peh->size);
    }

    return 0ULL;
}

static void arch_perfHandler(int signum, siginfo_t * si, void *unused)
{
    if (signum != SIGIO) {
        return;
    }
    if (si != NULL) {
        return;
    }
    if (unused) {
        return;
    }
    return;
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

bool arch_perfEnable(pid_t pid, honggfuzz_t * hfuzz, int *perfFd)
{
    if (hfuzz->createDynamically == false) {
        return true;
    }

    LOGMSG(l_DEBUG, "Enabling PERF for PID=%d", pid);

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.size = sizeof(struct perf_event_attr);
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.exclude_callchain_kernel = 1;
    pe.pinned = 1;

    switch (hfuzz->createDynamically) {
    case 'i':
        LOGMSG(l_DEBUG, "Using: PERF_COUNT_HW_INSTRUCTIONS for PID: %d", pid);
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        break;
    case 'b':
        LOGMSG(l_DEBUG, "Using: PERF_COUNT_HW_BRANCH_INSTRUCTIONS for PID: %d", pid);
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
        break;
    case 'e':
        LOGMSG(l_DEBUG, "Using: PERF_SAMPLE_BRANCH_STACK/PERF_SAMPLE_BRANCH_ANY for PID: %d", pid);
        pe.type = PERF_TYPE_HARDWARE;
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        pe.sample_type = PERF_SAMPLE_BRANCH_STACK;
        pe.sample_period = 100000;
        pe.branch_sample_type = PERF_SAMPLE_BRANCH_ANY;
        pe.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
        break;
    default:
        LOGMSG(l_ERROR, "Unknown perf mode: '%c' for PID: %d", hfuzz->createDynamically, pid);
        return false;
        break;
    }

    *perfFd = perf_event_open(&pe, pid, -1, -1, 0);
    if (*perfFd == -1) {
        LOGMSG_P(l_ERROR, "Error opening leader %llx", pe.config);
        return false;
    }

    if (hfuzz->createDynamically == 'e') {
        sigset_t smask;
        sigemptyset(&smask);
        struct sigaction sa = {
            .sa_handler = NULL,
            .sa_sigaction = arch_perfHandler,
            .sa_mask = smask,
            .sa_flags = SA_SIGINFO,
            .sa_restorer = NULL
        };

        if (sigaction(SIGIO, &sa, NULL) == -1) {
            LOGMSG_P(l_ERROR, "sigaction() failed");
            return false;
        }

        perfMmap = mmap(NULL, _HF_PERF_MMAP_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, *perfFd, 0);
        if (perfMmap == MAP_FAILED) {
            LOGMSG_P(l_ERROR, "mmap() failed");
            close(*perfFd);
            return false;
        }
#if 0
        if (fcntl(*perfFd, F_SETFL, O_RDWR | O_NONBLOCK | O_ASYNC) == -1) {
            LOGMSG_P(l_ERROR, "fnctl(F_SETFL)");
            close(*perfFd);
            return false;
        }

        struct f_owner_ex foe = {
            .type = F_OWNER_TID,
            .pid = syscall(__NR_gettid)
        };
        if (fcntl(*perfFd, F_SETSIG, SIGIO) == -1) {
            LOGMSG_P(l_ERROR, "fnctl(F_SETSIG)");
            close(*perfFd);
            return false;
        }

        if (fcntl(*perfFd, F_SETOWN_EX, &foe) == -1) {
            LOGMSG_P(l_ERROR, "fnctl(F_SETOWN_EX)");
            close(*perfFd);
            return false;
        }
#endif
    }

    if (ioctl(*perfFd, PERF_EVENT_IOC_RESET, 0) == -1) {
        LOGMSG_P(l_ERROR, "ioctl(perfFd='%d', PERF_EVENT_IOC_RESET) failed", perfFd);
        close(*perfFd);
        return false;
    }

    if (ioctl(*perfFd, PERF_EVENT_IOC_ENABLE, 0) == -1) {
        LOGMSG_P(l_ERROR, "ioctl(perfFd='%d', PERF_EVENT_IOC_ENABLE) failed", perfFd);
        close(*perfFd);
        return false;
    }
    return true;
}

void arch_perfAnalyze(honggfuzz_t * hfuzz, fuzzer_t * fuzzer, int perfFd)
{
    if (hfuzz->createDynamically == false) {
        return;
    }
    if (ioctl(perfFd, PERF_EVENT_IOC_DISABLE, 0) == -1) {
        LOGMSG_P(l_ERROR, "ioctl(perfFd='%d', PERF_EVENT_IOC_DISABLE) failed", perfFd);
        goto out;
    }

    uint64_t count = 0LL;
    if (hfuzz->createDynamically == 'e') {
        if (perfOverflow) {
            LOGMSG(l_WARN, "LBR has been overflown");
            goto out;
        } else {
            count = arch_perfParseMmap();
        }
    } else {
        if (read(perfFd, &count, sizeof(count)) == sizeof(count)) {
        } else {
            LOGMSG_P(l_ERROR, "read(perfFd='%d') failed", perfFd);
            goto out;
        }
    }
    fuzzer->branchCnt = count;

    LOGMSG(l_INFO,
           "Executed %lld branch instructions (best: %lld), fileSz: '%zu', bestFileSz: '%zu'",
           count, hfuzz->branchBestCnt, fuzzer->dynamicFileSz, hfuzz->dynamicFileBestSz);

 out:
    if (perfMmap != NULL) {
        munmap(perfMmap, _HF_PERF_MMAP_SZ);
    }
    close(perfFd);
    return;
}

// iOS build only, force-included ahead of Cemu's Common/precompiled.h: Cemu keys its Apple code
// paths (byte swapping, thread names, timers, Metal) off BOOST_OS_MACOS, which boost.predef leaves
// at 0 on iOS (it reports BOOST_OS_IOS instead). iOS is the same Darwin userland for everything
// Cemu uses, so present it as macOS; the few AppKit-only spots are replaced in ios/src.
#pragma once
#include <boost/predef/os.h>
#if BOOST_OS_IOS
#undef BOOST_OS_MACOS
#define BOOST_OS_MACOS BOOST_OS_IOS
#endif

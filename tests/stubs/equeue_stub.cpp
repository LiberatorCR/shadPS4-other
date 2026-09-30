// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/asio/io_context.hpp>
#include "core/file_sys/fs.h"
#include "core/loader/symbols_resolver.h"

void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, char const* nid, char const* lib,
                    u16 libversion, char const* mod, u64 symbol,
                    Core::Loader::SymbolType sym_type) {}

namespace Libraries::Kernel {
boost::asio::io_context io_context;
void KernelSignalRequest() {}
int* PS4_SYSV_ABI __Error() {
    static thread_local int error;
    return &error;
}
}

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const SymbolResolver&, u64) {}
}

namespace Core::FileSys {
// Handle allocation and guest exports are not exercised by EqueueInternal tests.
int HandleTable::CreateHandle() { return 0; }
File* HandleTable::GetFile(int) { return nullptr; }
void HandleTable::DeleteHandle(int) {}
}

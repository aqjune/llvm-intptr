//===-- WindowsResourceDumper.cpp - Windows Resource printer --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the Windows resource (.res) dumper for llvm-readobj.
//
//===----------------------------------------------------------------------===//

#include "WindowsResourceDumper.h"
#include "Error.h"
#include "llvm-readobj.h"
#include "llvm/Object/WindowsResource.h"
#include "llvm/Support/ScopedPrinter.h"

namespace llvm {
namespace object {
namespace WindowsRes {

std::string stripUTF16(const ArrayRef<UTF16> &UTF16Str) {
  std::string Result;
  Result.reserve(UTF16Str.size());

  for (UTF16 Ch : UTF16Str) {
    if (Ch <= 0xFF)
      Result += Ch;
    else
      Result += '?';
  }
  return Result;
}

Error Dumper::printData() {
  auto EntryPtrOrErr = WinRes->getHeadEntry();
  if (!EntryPtrOrErr)
    return EntryPtrOrErr.takeError();
  auto EntryPtr = *EntryPtrOrErr;

  bool IsEnd = false;
  while (!IsEnd) {
    printEntry(EntryPtr);

    if (auto Err = EntryPtr.moveNext(IsEnd))
      return Err;
  }
  return Error::success();
}

void Dumper::printEntry(const ResourceEntryRef &Ref) {
  if (Ref.checkTypeString()) {
    auto NarrowStr = stripUTF16(Ref.getTypeString());
    SW.printString("Resource type (string)", NarrowStr);
  } else
    SW.printNumber("Resource type (int)", Ref.getTypeID());

  if (Ref.checkNameString()) {
    auto NarrowStr = stripUTF16(Ref.getNameString());
    SW.printString("Resource name (string)", NarrowStr);
  } else
    SW.printNumber("Resource name (int)", Ref.getNameID());

  SW.printNumber("Data version", Ref.getDataVersion());
  SW.printHex("Memory flags", Ref.getMemoryFlags());
  SW.printNumber("Language ID", Ref.getLanguage());
  SW.printNumber("Version (major)", Ref.getMajorVersion());
  SW.printNumber("Version (minor)", Ref.getMinorVersion());
  SW.printNumber("Characteristics", Ref.getCharacteristics());
  SW.printNumber("Data size", Ref.getData().size());
  SW.printBinary("Data:", Ref.getData());
  SW.startLine() << "\n";
}

} // namespace WindowsRes
} // namespace object
} // namespace llvm

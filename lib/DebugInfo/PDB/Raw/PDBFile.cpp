//===- PDBFile.cpp - Low level interface to a PDB file ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Raw/PDBFile.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/MSF/StreamArray.h"
#include "llvm/DebugInfo/MSF/StreamInterface.h"
#include "llvm/DebugInfo/MSF/StreamReader.h"
#include "llvm/DebugInfo/MSF/StreamWriter.h"
#include "llvm/DebugInfo/PDB/Raw/DbiStream.h"
#include "llvm/DebugInfo/PDB/Raw/InfoStream.h"
#include "llvm/DebugInfo/PDB/Raw/NameHashTable.h"
#include "llvm/DebugInfo/PDB/Raw/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Raw/RawError.h"
#include "llvm/DebugInfo/PDB/Raw/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Raw/TpiStream.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::msf;
using namespace llvm::pdb;

namespace {
typedef FixedStreamArray<support::ulittle32_t> ulittle_array;
}

PDBFile::PDBFile(std::unique_ptr<ReadableStream> PdbFileBuffer,
                 BumpPtrAllocator &Allocator)
    : Allocator(Allocator), Buffer(std::move(PdbFileBuffer)) {}

PDBFile::~PDBFile() {}

uint32_t PDBFile::getBlockSize() const { return ContainerLayout.SB->BlockSize; }

uint32_t PDBFile::getFreeBlockMapBlock() const {
  return ContainerLayout.SB->FreeBlockMapBlock;
}

uint32_t PDBFile::getBlockCount() const {
  return ContainerLayout.SB->NumBlocks;
}

uint32_t PDBFile::getNumDirectoryBytes() const {
  return ContainerLayout.SB->NumDirectoryBytes;
}

uint32_t PDBFile::getBlockMapIndex() const {
  return ContainerLayout.SB->BlockMapAddr;
}

uint32_t PDBFile::getUnknown1() const { return ContainerLayout.SB->Unknown1; }

uint32_t PDBFile::getNumDirectoryBlocks() const {
  return msf::bytesToBlocks(ContainerLayout.SB->NumDirectoryBytes,
                            ContainerLayout.SB->BlockSize);
}

uint64_t PDBFile::getBlockMapOffset() const {
  return (uint64_t)ContainerLayout.SB->BlockMapAddr *
         ContainerLayout.SB->BlockSize;
}

uint32_t PDBFile::getNumStreams() const {
  return ContainerLayout.StreamSizes.size();
}

uint32_t PDBFile::getStreamByteSize(uint32_t StreamIndex) const {
  return ContainerLayout.StreamSizes[StreamIndex];
}

ArrayRef<support::ulittle32_t>
PDBFile::getStreamBlockList(uint32_t StreamIndex) const {
  return ContainerLayout.StreamMap[StreamIndex];
}

uint32_t PDBFile::getFileSize() const { return Buffer->getLength(); }

Expected<ArrayRef<uint8_t>> PDBFile::getBlockData(uint32_t BlockIndex,
                                                  uint32_t NumBytes) const {
  uint64_t StreamBlockOffset = msf::blockToOffset(BlockIndex, getBlockSize());

  ArrayRef<uint8_t> Result;
  if (auto EC = Buffer->readBytes(StreamBlockOffset, NumBytes, Result))
    return std::move(EC);
  return Result;
}

Error PDBFile::setBlockData(uint32_t BlockIndex, uint32_t Offset,
                            ArrayRef<uint8_t> Data) const {
  return make_error<RawError>(raw_error_code::not_writable,
                              "PDBFile is immutable");
}

Error PDBFile::parseFileHeaders() {
  StreamReader Reader(*Buffer);

  // Initialize SB.
  const msf::SuperBlock *SB = nullptr;
  if (auto EC = Reader.readObject(SB)) {
    consumeError(std::move(EC));
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Does not contain superblock");
  }

  if (auto EC = msf::validateSuperBlock(*SB))
    return EC;

  if (Buffer->getLength() % SB->BlockSize != 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "File size is not a multiple of block size");
  ContainerLayout.SB = SB;

  // Initialize Free Page Map.
  ContainerLayout.FreePageMap.resize(getBlockSize() * 8);
  uint64_t FPMOffset = SB->FreeBlockMapBlock * getBlockSize();
  ArrayRef<uint8_t> FPMBlock;
  if (auto EC = Buffer->readBytes(FPMOffset, getBlockSize(), FPMBlock))
    return EC;
  for (uint32_t I = 0, E = getBlockSize() * 8; I != E; ++I)
    if (FPMBlock[I / 8] & (1 << (I % 8)))
      ContainerLayout.FreePageMap[I] = true;

  Reader.setOffset(getBlockMapOffset());
  if (auto EC = Reader.readArray(ContainerLayout.DirectoryBlocks,
                                 getNumDirectoryBlocks()))
    return EC;

  return Error::success();
}

Error PDBFile::parseStreamData() {
  assert(ContainerLayout.SB);
  if (DirectoryStream)
    return Error::success();

  uint32_t NumStreams = 0;

  // Normally you can't use a MappedBlockStream without having fully parsed the
  // PDB file, because it accesses the directory and various other things, which
  // is exactly what we are attempting to parse.  By specifying a custom
  // subclass of IPDBStreamData which only accesses the fields that have already
  // been parsed, we can avoid this and reuse MappedBlockStream.
  auto DS = MappedBlockStream::createDirectoryStream(ContainerLayout, *Buffer);
  StreamReader Reader(*DS);
  if (auto EC = Reader.readInteger(NumStreams))
    return EC;

  if (auto EC = Reader.readArray(ContainerLayout.StreamSizes, NumStreams))
    return EC;
  for (uint32_t I = 0; I < NumStreams; ++I) {
    uint32_t StreamSize = getStreamByteSize(I);
    // FIXME: What does StreamSize ~0U mean?
    uint64_t NumExpectedStreamBlocks =
        StreamSize == UINT32_MAX
            ? 0
            : msf::bytesToBlocks(StreamSize, ContainerLayout.SB->BlockSize);

    // For convenience, we store the block array contiguously.  This is because
    // if someone calls setStreamMap(), it is more convenient to be able to call
    // it with an ArrayRef instead of setting up a StreamRef.  Since the
    // DirectoryStream is cached in the class and thus lives for the life of the
    // class, we can be guaranteed that readArray() will return a stable
    // reference, even if it has to allocate from its internal pool.
    ArrayRef<support::ulittle32_t> Blocks;
    if (auto EC = Reader.readArray(Blocks, NumExpectedStreamBlocks))
      return EC;
    for (uint32_t Block : Blocks) {
      uint64_t BlockEndOffset =
          (uint64_t)(Block + 1) * ContainerLayout.SB->BlockSize;
      if (BlockEndOffset > getFileSize())
        return make_error<RawError>(raw_error_code::corrupt_file,
                                    "Stream block map is corrupt.");
    }
    ContainerLayout.StreamMap.push_back(Blocks);
  }

  // We should have read exactly SB->NumDirectoryBytes bytes.
  assert(Reader.bytesRemaining() == 0);
  DirectoryStream = std::move(DS);
  return Error::success();
}

llvm::ArrayRef<support::ulittle32_t> PDBFile::getDirectoryBlockArray() const {
  return ContainerLayout.DirectoryBlocks;
}

Expected<InfoStream &> PDBFile::getPDBInfoStream() {
  if (!Info) {
    auto InfoS = MappedBlockStream::createIndexedStream(ContainerLayout,
                                                        *Buffer, StreamPDB);
    auto TempInfo = llvm::make_unique<InfoStream>(std::move(InfoS));
    if (auto EC = TempInfo->reload())
      return std::move(EC);
    Info = std::move(TempInfo);
  }
  return *Info;
}

Expected<DbiStream &> PDBFile::getPDBDbiStream() {
  if (!Dbi) {
    auto DbiS = MappedBlockStream::createIndexedStream(ContainerLayout, *Buffer,
                                                       StreamDBI);
    auto TempDbi = llvm::make_unique<DbiStream>(*this, std::move(DbiS));
    if (auto EC = TempDbi->reload())
      return std::move(EC);
    Dbi = std::move(TempDbi);
  }
  return *Dbi;
}

Expected<TpiStream &> PDBFile::getPDBTpiStream() {
  if (!Tpi) {
    auto TpiS = MappedBlockStream::createIndexedStream(ContainerLayout, *Buffer,
                                                       StreamTPI);
    auto TempTpi = llvm::make_unique<TpiStream>(*this, std::move(TpiS));
    if (auto EC = TempTpi->reload())
      return std::move(EC);
    Tpi = std::move(TempTpi);
  }
  return *Tpi;
}

Expected<TpiStream &> PDBFile::getPDBIpiStream() {
  if (!Ipi) {
    auto IpiS = MappedBlockStream::createIndexedStream(ContainerLayout, *Buffer,
                                                       StreamIPI);
    auto TempIpi = llvm::make_unique<TpiStream>(*this, std::move(IpiS));
    if (auto EC = TempIpi->reload())
      return std::move(EC);
    Ipi = std::move(TempIpi);
  }
  return *Ipi;
}

Expected<PublicsStream &> PDBFile::getPDBPublicsStream() {
  if (!Publics) {
    auto DbiS = getPDBDbiStream();
    if (!DbiS)
      return DbiS.takeError();

    uint32_t PublicsStreamNum = DbiS->getPublicSymbolStreamIndex();

    auto PublicS = MappedBlockStream::createIndexedStream(
        ContainerLayout, *Buffer, PublicsStreamNum);
    auto TempPublics =
        llvm::make_unique<PublicsStream>(*this, std::move(PublicS));
    if (auto EC = TempPublics->reload())
      return std::move(EC);
    Publics = std::move(TempPublics);
  }
  return *Publics;
}

Expected<SymbolStream &> PDBFile::getPDBSymbolStream() {
  if (!Symbols) {
    auto DbiS = getPDBDbiStream();
    if (!DbiS)
      return DbiS.takeError();

    uint32_t SymbolStreamNum = DbiS->getSymRecordStreamIndex();
    auto SymbolS = MappedBlockStream::createIndexedStream(
        ContainerLayout, *Buffer, SymbolStreamNum);

    auto TempSymbols = llvm::make_unique<SymbolStream>(std::move(SymbolS));
    if (auto EC = TempSymbols->reload())
      return std::move(EC);
    Symbols = std::move(TempSymbols);
  }
  return *Symbols;
}

Expected<NameHashTable &> PDBFile::getStringTable() {
  if (!StringTable || !StringTableStream) {
    auto IS = getPDBInfoStream();
    if (!IS)
      return IS.takeError();

    uint32_t NameStreamIndex = IS->getNamedStreamIndex("/names");

    if (NameStreamIndex == 0)
      return make_error<RawError>(raw_error_code::no_stream);
    if (NameStreamIndex >= getNumStreams())
      return make_error<RawError>(raw_error_code::no_stream);
    auto NS = MappedBlockStream::createIndexedStream(ContainerLayout, *Buffer,
                                                     NameStreamIndex);

    StreamReader Reader(*NS);
    auto N = llvm::make_unique<NameHashTable>();
    if (auto EC = N->load(Reader))
      return std::move(EC);
    StringTable = std::move(N);
    StringTableStream = std::move(NS);
  }
  return *StringTable;
}

#pragma once

#include "Sqex.h"

namespace Sqex::Sqpack {
	static constexpr uint32_t EntryAlignment = 128;

	template<typename T, typename CountT = T>
	struct AlignResult {
		CountT Count;
		T Alloc;
		T Pad;

		operator T() const {
			return Alloc;
		}
	};

	template<typename T, typename CountT = T>
	AlignResult<T, CountT> Align(T value, T by = static_cast<T>(EntryAlignment)) {
		const auto count = (value + by - 1) / by;
		const auto alloc = count * by;
		const auto pad = alloc - value;
		return {
			.Count = static_cast<CountT>(count),
			.Alloc = static_cast<T>(alloc),
			.Pad = static_cast<T>(pad),
		};
	}

	struct Sha1Value {
		char Value[20]{};

		void Verify(const void* data, size_t size, const char* errorMessage) const;
		template<typename T>
		void Verify(std::span<T> data, const char* errorMessage) const {
			Verify(data.data(), data.size_bytes(), errorMessage);
		}

		void SetFromPointer(const void* data, size_t size);
		template<typename T>
		void SetFrom(std::span<T> data) {
			SetFromPointer(data.data(), data.size_bytes());
		}
		template<typename ...Args>
		void SetFromSpan(Args...args) {
			SetFrom(std::span(std::forward<Args>(args)...));
		}

		bool operator==(const Sha1Value& r) const;
		bool operator!=(const Sha1Value& r) const;
		bool operator==(const char(&r)[20]) const;
		bool operator!=(const char(&r)[20]) const;

		[[nodiscard]] bool IsZero() const;
	};

	class CorruptDataException : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};

	enum class SqpackType : uint32_t {
		Unspecified = UINT32_MAX,
		SqDatabase = 0,
		SqData = 1,
		SqIndex = 2,
	};

	struct SqpackHeader {
		static constexpr uint32_t Unknown1_Value = 1;
		static constexpr uint32_t Unknown2_Value = 0xFFFFFFFFUL;
		static const char Signature_Value[12];

		char Signature[12]{};
		LE<uint32_t> HeaderSize;
		LE<uint32_t> Unknown1;  // 1
		LE<SqpackType> Type;
		LE<uint32_t> YYYYMMDD;
		LE<uint32_t> Time;
		LE<uint32_t> Unknown2; // Intl: 0xFFFFFFFF, KR/CN: 1
		char Padding_0x024[0x3c0 - 0x024]{};
		Sha1Value Sha1;
		char Padding_0x3D4[0x2c]{};

		void VerifySqpackHeader(SqpackType supposedType);
	};
	static_assert(offsetof(SqpackHeader, Sha1) == 0x3c0, "Bad SqpackHeader definition");
	static_assert(sizeof(SqpackHeader) == 1024);
	
	namespace SqIndex {
		struct SegmentDescriptor {
			LE<uint32_t> Count;
			LE<uint32_t> Offset;
			LE<uint32_t> Size;
			Sha1Value Sha1;
			char Padding_0x020[0x28]{};
		};
		static_assert(sizeof SegmentDescriptor == 0x48);

		/*
		 * Segment 1
		 * * Stands for files
		 * * Descriptor.Count = 1
		 *
		 * Segment 2
		 * * Descriptor.Count stands for number of .dat files
		 * * Descriptor.Size is always 0x100
		 * * Data is always 8x00s, 4xFFs, and the rest is 0x00s
		 *
		 * Segment 3
		 * * Descriptor.Count = 0
		 *
		 * Segment 4
		 * * Stands for folders
		 * * Descriptor.Count = 0
		 */

		struct Header {
			enum class IndexType : uint32_t {
				Unspecified = UINT32_MAX,
				Index = 0,
				Index2 = 2,
			};

			LE<uint32_t> HeaderSize;
			SegmentDescriptor FileSegment;
			char Padding_0x04C[4]{};
			SegmentDescriptor DataFilesSegment;  // Size is always 0x100
			SegmentDescriptor UnknownSegment3;
			SegmentDescriptor FolderSegment;
			char Padding_0x128[4]{};
			LE<IndexType> Type;
			char Padding_0x130[0x3c0 - 0x130]{};
			Sha1Value Sha1;
			char Padding_0x3D4[0x2c]{};

			void VerifySqpackIndexHeader(IndexType expectedIndexType) const;

			void VerifyDataFileSegment(const std::vector<char>& DataFileSegment) const;

		};
		static_assert(sizeof(Header) == 1024);

		struct LEDataLocator : LE<uint32_t> {
			using LE<uint32_t>::LE;
			LEDataLocator(uint32_t index, uint64_t offset);

			[[nodiscard]] uint32_t Index() const { return (Value() & 0xF) / 2; }
			[[nodiscard]] uint64_t Offset() const { return (Value() & 0xFFFFFFF0UL) * 8ULL; }
			uint32_t Index(uint32_t value);
			uint64_t Offset(uint64_t value);
		};

		struct FileSegmentEntry {
			LE<uint32_t> NameHash;
			LE<uint32_t> PathHash;
			LEDataLocator DatFile;
			LE<uint32_t> Padding;
		};

		struct FileSegmentEntry2 {
			LE<uint32_t> FullPathHash;
			LEDataLocator DatFile;
		};

		struct Segment3Entry {
			LE<uint32_t> Unknown1;
			LE<uint32_t> Unknown2;
			LE<uint32_t> Unknown3;
			LE<uint32_t> Unknown4;
		};

		struct FolderSegmentEntry {
			LE<uint32_t> NameHash;
			LE<uint32_t> FileSegmentOffset;
			LE<uint32_t> FileSegmentSize;
			LE<uint32_t> Padding;

			void Verify() const;
		};
	}

	namespace SqData {
		struct Header {
			static constexpr uint32_t MaxFileSize_Value = 0x77359400;  // 2GB
			static constexpr uint64_t MaxFileSize_MaxValue = 0x800000000ULL;  // 32GiB, maximum addressable via how LEDataLocator works
			static constexpr uint32_t Unknown1_Value = 0x10;

			LE<uint32_t> HeaderSize;
			LE<uint32_t> Null1;
			LE<uint32_t> Unknown1;
			union DataSizeDivBy8Type {
				LE<uint32_t> RawValue;

				DataSizeDivBy8Type& operator=(uint64_t value) {
					if (value % 128)
						throw std::invalid_argument("Value must be a multiple of 8.");
					if (value / 128ULL > UINT32_MAX)
						throw std::invalid_argument("Value too big.");
					RawValue = static_cast<uint32_t>(value / 128ULL);
					return *this;
				}

				operator uint64_t() const {
					return Value();
				}

				[[nodiscard]] uint64_t Value() const {
					return RawValue * 128ULL;
				}
			} DataSize;  // From end of this header to EOF
			LE<uint32_t> SpanIndex;  // 0x01 = .dat0, 0x02 = .dat1, 0x03 = .dat2, ...
			LE<uint32_t> Null2;
			LE<uint64_t> MaxFileSize;
			Sha1Value DataSha1;  // From end of this header to EOF
			char Padding_0x034[0x3c0 - 0x034]{};
			Sha1Value Sha1;
			char Padding_0x3D4[0x2c]{};

			void Verify(uint32_t expectedSpanIndex) const;
		};
		static_assert(offsetof(Header, Sha1) == 0x3c0, "Bad SqDataHeader definition");

		enum class FileEntryType {
			Empty = 1,
			Binary = 2,
			Model = 3,
			Texture = 4,
		};

		struct BlockHeaderLocator {
			LE<uint32_t> Offset;
			LE<uint16_t> BlockSize;
			LE<uint16_t> DecompressedDataSize;
		};

		struct BlockHeader {
			static constexpr uint32_t CompressedSizeNotCompressed = 32000;
			LE<uint32_t> HeaderSize;
			LE<uint32_t> Version;
			LE<uint32_t> CompressedSize;
			LE<uint32_t> DecompressedSize;
		};

		struct FileEntryHeader {
			LE<uint32_t> HeaderSize;
			LE<FileEntryType> Type;
			LE<uint32_t> DecompressedSize;
			LE<uint32_t> Unknown1;
			LE<uint32_t> BlockBufferSize;
			LE<uint32_t> BlockCountOrVersion;
		};

		struct TextureBlockHeaderLocator {
			LE<uint32_t> FirstBlockOffset;
			LE<uint32_t> TotalSize;
			LE<uint32_t> DecompressedSize;
			LE<uint32_t> FirstSubBlockIndex;
			LE<uint32_t> SubBlockCount;
		};
		
		struct ModelBlockLocator {
			template<typename T>
			struct ChunkInfo {
				T Stack;
				T Runtime;
				T Vertex[3];
				T EdgeGeometryVertex[3];
				T Index[3];
			};

			ChunkInfo<LE<uint32_t>> DecompressedSizes;
			ChunkInfo<LE<uint32_t>> ChunkSizes;
			ChunkInfo<LE<uint32_t>> FirstBlockOffsets;
			ChunkInfo<LE<uint16_t>> FirstBlockIndices;
			ChunkInfo<LE<uint16_t>> BlockCount;
			LE<uint16_t> VertexDeclarationCount;
			LE<uint16_t> MaterialCount;
			LE<uint8_t> LodCount;
			LE<uint8_t> EnableIndexBufferStreaming;
			LE<uint8_t> EnableEdgeGeometry;
			LE<uint8_t> Padding;
		};
		static_assert(sizeof ModelBlockLocator == 184);
	}

	extern const uint32_t SqexHashTable[4][256];
	uint32_t SqexHash(const char* data, size_t len);
	uint32_t SqexHash(const std::string& text);
	uint32_t SqexHash(const std::string_view& text);
	uint32_t SqexHash(const std::filesystem::path& path);
}
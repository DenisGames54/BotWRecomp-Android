#pragma once

#include "h264_compat.h"
#include "parser/H264Parser.h"

namespace H264
{
	class H264DecoderBackend
	{
	  protected:
		struct DataToDecode
		{
			uint8* m_data;
			uint32 m_length;
			std::vector<uint8> m_buffer;
		};

		static constexpr uint32 CMD_FLUSH = 0xFFFFFFFF;

	  public:
		struct DecodeResult
		{
			bool isDecoded{false};
			bool hasFrame{false}; // set to true if a full frame was successfully decoded
			double timestamp{};
			void* imageOutput{nullptr};
			sint32 frameWidth{0};
			sint32 frameHeight{0};
			uint32 bytesPerRow{0};
			bool cropEnable{false};
			sint32 cropTop{0};
			sint32 cropBottom{0};
			sint32 cropLeft{0};
			sint32 cropRight{0};
		};

		struct DecodedSlice
		{
			bool isUsed{false};
			DecodeResult result;
			DataToDecode dataToDecode;
		};

		H264DecoderBackend() = default;
		virtual ~H264DecoderBackend() = default;

		virtual void Init(bool isBufferedMode) = 0;
		virtual void Destroy() = 0;

		void QueueForDecode(uint8* data, uint32 length, double timestamp, void* imagePtr)
		{
			std::unique_lock _l(m_decodeQueueMtx);

			DecodedSlice& ds = GetFreeDecodedSliceEntry();

			ds.dataToDecode.m_buffer.assign(data, data + length);
			ds.dataToDecode.m_data = ds.dataToDecode.m_buffer.data();
			ds.dataToDecode.m_length = length;

			ds.result.isDecoded = false;
			ds.result.imageOutput = imagePtr;
			ds.result.timestamp = timestamp;

			m_decodeQueue.push_back(std::distance(m_decodedSliceArray.data(), &ds));
			m_decodeSem.increment();
		}

		void QueueFlush()
		{
			std::unique_lock _l(m_decodeQueueMtx);
			m_decodeQueue.push_back(CMD_FLUSH);
			m_decodeSem.increment();
		}

		bool GetFrameOutputIfReady(DecodeResult& result)
		{
			std::unique_lock _l(m_decodeQueueMtx);
			if(m_displayQueue.empty())
				return false;
			uint32 sliceIndex = m_displayQueue.front();
			DecodedSlice& ds = m_decodedSliceArray[sliceIndex];
			cemu_assert_debug(ds.result.isDecoded);
			std::swap(result, ds.result);
			ds.isUsed = false;
			m_displayQueue.erase(m_displayQueue.begin());
			return true;
		}

		H264Event& GetFrameOutputEvent()
		{
			return m_displayQueueEvt;
		}

		H264Event& GetFlushEvent()
		{
			return m_flushEvt;
		}

	  protected:
		DecodedSlice& GetFreeDecodedSliceEntry()
		{
			for (auto& slice : m_decodedSliceArray)
			{
				if (!slice.isUsed)
				{
					slice.isUsed = true;
					return slice;
				}
			}
			cemu_assert_suspicious();
			return m_decodedSliceArray[0];
		}

		std::mutex m_decodeQueueMtx;
		std::vector<uint32> m_decodeQueue; // indices into m_decodedSliceArray, in order of decode input
		CounterSemaphore m_decodeSem;
		std::vector<uint32> m_displayQueue; // indices into m_decodedSliceArray, in order of frame display output
		H264Event m_displayQueueEvt; // signalled when a new frame is ready for display
		H264Event m_flushEvt; // signalled after flush operation finished and all queued slices are decoded

		// frame output queue
		std::mutex m_frameOutputMtx;
		std::array<DecodedSlice, 32> m_decodedSliceArray;
	};
}
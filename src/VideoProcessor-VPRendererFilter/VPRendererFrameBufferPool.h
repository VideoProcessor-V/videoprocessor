#pragma once

// VP Renderer DirectShow filter: reference-counted frame copies handed to VP
// as VideoFrame source buffers. A released buffer returns to the pool so a
// steady stream reuses its allocations; buffers of another size (a format
// change) are freed. Lifetime is shared with outstanding buffers: VP may hold
// a frame after the filter released the pool.

#include <Windows.h>
#include <Unknwn.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>


namespace VPRendererFilter
{
	class FrameBufferPool
	{
	private:
		struct State;

	public:
		class Buffer final : public IUnknown
		{
		public:
			~Buffer() = default;
			uint8_t* Data() { return m_bytes.data(); }
			size_t Size() const { return m_bytes.size(); }

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override
			{
				if (!ppv)
					return E_POINTER;
				if (iid == IID_IUnknown)
				{
					*ppv = static_cast<IUnknown*>(this);
					AddRef();
					return S_OK;
				}
				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			ULONG STDMETHODCALLTYPE AddRef() override
			{
				return ++m_refCount;
			}

			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG count = --m_refCount;
				if (count == 0)
				{
					std::shared_ptr<State> state = m_state;
					m_state.reset();
					if (!state || !state->Return(this))
						delete this;
				}
				return count;
			}

		private:
			friend class FrameBufferPool;
			explicit Buffer(size_t size) : m_bytes(size) {}

			std::vector<uint8_t> m_bytes;
			std::atomic<ULONG> m_refCount{ 0 };
			std::shared_ptr<State> m_state;
		};

		// Free buffers kept for reuse; the in-flight count is bounded by VP's
		// queue, so this only bounds idle memory.
		explicit FrameBufferPool(size_t maxIdleBuffers = 4) :
			m_state(std::make_shared<State>(maxIdleBuffers)) {}

		~FrameBufferPool()
		{
			m_state->Clear();
		}

		// A buffer of exactly size bytes with one reference owned by the caller.
		Buffer* Acquire(size_t size)
		{
			Buffer* buffer = m_state->Take(size);
			if (!buffer)
				buffer = new Buffer(size);
			buffer->m_state = m_state;
			buffer->m_refCount = 1;
			return buffer;
		}

	private:
		struct State
		{
			explicit State(size_t maxIdle) : maxIdle(maxIdle) {}
			~State() { Clear(); }

			Buffer* Take(size_t size)
			{
				std::lock_guard<std::mutex> lock(mutex);
				for (size_t i = 0; i < idle.size(); ++i)
				{
					if (idle[i]->Size() == size)
					{
						Buffer* buffer = idle[i];
						idle.erase(idle.begin() + static_cast<std::ptrdiff_t>(i));
						return buffer;
					}
				}
				return nullptr;
			}

			bool Return(Buffer* buffer)
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (closed || idle.size() >= maxIdle)
					return false;
				// A different size means the format changed: drop the old ones.
				if (!idle.empty() && idle.front()->Size() != buffer->Size())
				{
					for (Buffer* old : idle)
						delete old;
					idle.clear();
				}
				idle.push_back(buffer);
				return true;
			}

			void Clear()
			{
				std::lock_guard<std::mutex> lock(mutex);
				closed = true;
				for (Buffer* buffer : idle)
					delete buffer;
				idle.clear();
			}

			std::mutex mutex;
			std::vector<Buffer*> idle;
			size_t maxIdle;
			bool closed = false;
		};

		std::shared_ptr<State> m_state;
	};
}

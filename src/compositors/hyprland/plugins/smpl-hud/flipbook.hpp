#pragma once

// Frames of a picture that repeats, kept for every window that shows it. A picture that comes round
// again every few seconds need be made only once: the first time a frame is wanted it is baked and
// put here, and from then on it is looked up. Nothing here touches Hyprland: what a frame is, and how
// it is made, is for the caller; this only keeps count of what there is room for.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace smplhud {

template <class Frame>
class Flipbook {
  public:
    // Up to `budget` bytes are kept in all. A picture that nobody has asked for in `idle` seconds is
    // dropped to make room for another; one that is being asked for never is, so two pictures that do
    // not fit together do not throw each other out again and again.
    Flipbook(size_t budget, double idle) : m_budget(budget), m_idle(idle) {}

    // Frame `index` of the picture `page` if it is kept, else none. The pointer holds until the next call.
    const Frame* find(const std::string& page, size_t index, double now) {
        const auto it = m_pages.find(page);
        if (it == m_pages.end())
            return nullptr;

        it->second.seen = now;
        return index < it->second.slots.size() && it->second.slots[index] ? &*it->second.slots[index] : nullptr;
    }

    // Keeps a frame that has just been baked, `bytes` large, as frame `index` of a picture of `frames`
    // frames. None if there is no room for it, and then nothing is kept. The pointer holds until the next call.
    // A page asked for with another number of frames is another picture, and what it held is dropped.
    const Frame* keep(const std::string& page, size_t index, size_t frames, Frame frame, size_t bytes, double now) {
        if (index >= frames || bytes > m_budget)
            return nullptr;

        auto it = m_pages.find(page);
        if (it != m_pages.end() && it->second.slots.size() != frames) {
            m_bytes -= it->second.bytes;
            m_pages.erase(it);
            it = m_pages.end();
        }
        if (it != m_pages.end()) {
            it->second.seen = now;
            if (it->second.slots[index])
                return &*it->second.slots[index];
        }

        while (m_bytes + bytes > m_budget) {
            auto stalest = m_pages.end();
            for (auto other = m_pages.begin(); other != m_pages.end(); ++other)
                if (other->first != page && now - other->second.seen >= m_idle && (stalest == m_pages.end() || other->second.seen < stalest->second.seen))
                    stalest = other;
            if (stalest == m_pages.end())
                return nullptr;

            m_bytes -= stalest->second.bytes;
            m_pages.erase(stalest);
        }

        auto& p = m_pages[page];
        if (p.slots.empty())
            p.slots.resize(frames);
        p.seen         = now;
        p.slots[index] = std::move(frame);
        p.bytes       += bytes;
        m_bytes       += bytes;
        return &*p.slots[index];
    }

    size_t bytes() const { return m_bytes; }
    size_t pages() const { return m_pages.size(); }

    void clear() {
        m_pages.clear();
        m_bytes = 0;
    }

  private:
    struct Page {
        std::vector<std::optional<Frame>> slots;
        size_t                            bytes = 0;
        double                            seen  = 0;
    };

    std::map<std::string, Page> m_pages;
    size_t                      m_budget, m_bytes = 0;
    double                      m_idle;
};

} // namespace smplhud

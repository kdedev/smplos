#include "../flipbook.hpp"
#include "check.hpp"

#include <string>

using namespace smplhud;

namespace {

using Book = Flipbook<std::string>;

void testFindAndKeep() {
    Book book(1000, 10);
    CHECK(book.pages() == 0 && book.bytes() == 0);
    CHECK(book.find("a", 0, 0) == nullptr);

    const std::string* kept = book.keep("a", 3, 10, "three", 40, 1);
    CHECK(kept && *kept == "three");
    CHECK(book.pages() == 1 && book.bytes() == 40);

    const std::string* found = book.find("a", 3, 2);
    CHECK(found && *found == "three");

    // only what was kept is there
    CHECK(book.find("a", 2, 2) == nullptr);
    CHECK(book.find("a", 10, 2) == nullptr);
    CHECK(book.find("b", 3, 2) == nullptr);

    // a frame that is there is not made again, nor counted twice
    const std::string* again = book.keep("a", 3, 10, "other", 40, 3);
    CHECK(again && *again == "three");
    CHECK(book.bytes() == 40);

    // a page holds many frames, and many pages are kept
    CHECK(book.keep("a", 4, 10, "four", 25, 3));
    CHECK(book.keep("b", 0, 5, "b0", 30, 3));
    CHECK(book.pages() == 2 && book.bytes() == 95);
    CHECK(*book.find("a", 4, 4) == "four" && *book.find("a", 3, 4) == "three" && *book.find("b", 0, 4) == "b0");
}

void testBadFrames() {
    Book book(100, 10);

    // a frame that is not in its page, and one that is larger than the whole, are not kept
    CHECK(book.keep("a", 10, 10, "x", 10, 0) == nullptr);
    CHECK(book.keep("a", 0, 0, "x", 10, 0) == nullptr);
    CHECK(book.keep("a", 0, 10, "huge", 101, 0) == nullptr);
    CHECK(book.pages() == 0 && book.bytes() == 0);

    // one that is exactly the whole is
    CHECK(book.keep("a", 0, 10, "whole", 100, 0));
    CHECK(book.bytes() == 100);
}

void testRoomIsLimited() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 40, 0));
    CHECK(book.keep("a", 1, 10, "a1", 40, 0));

    // no room for a third, and nobody that has been idle to make way: nothing is kept and nothing is lost
    CHECK(book.keep("a", 2, 10, "a2", 40, 1) == nullptr);
    CHECK(book.keep("b", 0, 10, "b0", 40, 1) == nullptr);
    CHECK(book.bytes() == 80 && book.pages() == 1);
    CHECK(*book.find("a", 0, 2) == "a0" && *book.find("a", 1, 2) == "a1");
    CHECK(book.find("b", 0, 2) == nullptr);

    // what is still small enough goes in
    CHECK(book.keep("a", 2, 10, "a2", 20, 2));
    CHECK(book.bytes() == 100);
}

void testIdlePagesMakeWay() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 40, 0));
    CHECK(book.keep("a", 1, 10, "a1", 40, 0));

    // not yet idle for long enough
    CHECK(book.keep("b", 0, 10, "b0", 40, 9.5) == nullptr);
    CHECK(book.pages() == 1);

    // idle for long enough: its room goes to the other
    CHECK(book.keep("b", 0, 10, "b0", 40, 10));
    CHECK(book.pages() == 1);
    CHECK(book.find("a", 0, 10) == nullptr);
    CHECK(*book.find("b", 0, 10) == "b0");
    CHECK(book.bytes() == 40);
}

void testLookingKeepsAPage() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 60, 0));

    // each look at a frame is a use of the page
    CHECK(book.find("a", 0, 8));
    CHECK(book.keep("b", 0, 10, "b0", 60, 15) == nullptr);
    CHECK(book.find("a", 0, 15));
    CHECK(book.keep("b", 0, 10, "b0", 60, 24) == nullptr);

    // missing a frame is a use too: the page is being asked for
    CHECK(book.find("a", 5, 30) == nullptr);
    CHECK(book.keep("b", 0, 10, "b0", 60, 39) == nullptr);

    // and when nobody asks for 10 s it makes way
    CHECK(book.keep("b", 0, 10, "b0", 60, 40));
    CHECK(book.find("a", 0, 40) == nullptr);
}

void testNeverEvictsTheRequestedPage() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 60, 0));

    // a is idle, but it is the one that wants room, and a page does not make way for itself
    CHECK(book.keep("a", 1, 10, "a1", 60, 50) == nullptr);
    CHECK(book.pages() == 1 && book.bytes() == 60);
    CHECK(*book.find("a", 0, 50) == "a0");
}

void testStalestGoesFirstAndOnlyAsMuchAsNeeded() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 50, 0));
    CHECK(book.keep("b", 0, 10, "b0", 50, 1));
    CHECK(book.bytes() == 100);

    // c needs the room of one page, and a was used last longest ago
    CHECK(book.keep("c", 0, 10, "c0", 50, 20));
    CHECK(book.find("a", 0, 20) == nullptr);
    CHECK(book.find("b", 0, 20));
    CHECK(book.find("c", 0, 20));
    CHECK(book.pages() == 2 && book.bytes() == 100);

    // and one that needs the room of both takes both
    Book more(100, 10);
    CHECK(more.keep("a", 0, 10, "a0", 50, 0));
    CHECK(more.keep("b", 0, 10, "b0", 50, 1));
    CHECK(more.keep("c", 0, 10, "c0", 90, 20));
    CHECK(more.pages() == 1 && more.bytes() == 90);
}

void testPageSizeChanges() {
    Book book(1000, 10);
    CHECK(book.keep("a", 2, 10, "old", 40, 0));
    CHECK(book.keep("a", 3, 10, "old3", 40, 0));
    CHECK(book.bytes() == 80);

    // the same page with another number of frames is another picture: what was in it is dropped
    const std::string* kept = book.keep("a", 2, 20, "new", 30, 1);
    CHECK(kept && *kept == "new");
    CHECK(book.bytes() == 30 && book.pages() == 1);
    CHECK(book.find("a", 3, 1) == nullptr);
    CHECK(*book.find("a", 2, 1) == "new");
}

void testClear() {
    Book book(100, 10);
    CHECK(book.keep("a", 0, 10, "a0", 40, 0));
    CHECK(book.keep("b", 0, 10, "b0", 40, 0));
    book.clear();
    CHECK(book.pages() == 0 && book.bytes() == 0);
    CHECK(book.find("a", 0, 1) == nullptr);

    // and it can be filled again to the brim
    CHECK(book.keep("c", 0, 10, "c0", 100, 1));
}

void testBytesAddUp() {
    Book book(1000, 10);
    size_t expected = 0;
    for (size_t i = 0; i < 30; ++i) {
        const size_t bytes = 10 + i;
        if (book.keep("page" + std::to_string(i % 3), i, 30, "x", bytes, double(i)))
            expected += bytes;
    }
    CHECK(book.bytes() == expected);
    CHECK(book.pages() == 3);
}

} // namespace

int main() {
    testFindAndKeep();
    testBadFrames();
    testRoomIsLimited();
    testIdlePagesMakeWay();
    testLookingKeepsAPage();
    testNeverEvictsTheRequestedPage();
    testStalestGoesFirstAndOnlyAsMuchAsNeeded();
    testPageSizeChanges();
    testClear();
    testBytesAddUp();
    return finish("test_flipbook");
}

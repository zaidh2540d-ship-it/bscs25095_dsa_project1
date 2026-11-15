#include <iostream>
#include <cstring>
#include <algorithm>
#include "helper.h"

int main()
{
    const char *msg = "123456789";
    CRC32 c;
    c.update(msg, 9);
    cout << hex << c.value() << " (expected cbf43926)" << endl;

    CRC32 s;
    char status = 0;
    s.update(&status, 1);
    s.update("hello world", 11);
    char trailer[4];
    encode32(s.value(), trailer);

    string wire = "hello world";
    wire.append(trailer, 4);

    TrailerChecker tc;
    tc.addPrefixByte(status);
    string payload;
    for (size_t i = 0; i < wire.size(); i += 3)
        payload += tc.feed(wire.data() + i, min<size_t>(3, wire.size() - i));

    cout << "payload=" << payload << " verify=" << tc.verify() << " (expected 1)" << endl;
}
// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <vector>
#include "../UsbDk/ConfigurationDescriptor.h"
#include "../UsbDk/ConfigurationTransaction.h"
#include "../UsbDk/TransferBounds.h"

static std::vector<unsigned char> configuration(unsigned char value)
{
    // Authored two-interface fixture; it contains no descriptors from a device.
    return {9,2,41,0,2,value,0,0x80,50,
            9,4,0,0,1,0xff,0,0,0, 7,5,0x81,2,64,0,0,
            9,4,2,0,1,0xff,0,0,0, 7,5,0x02,3,16,0,1};
}

int main()
{
    UsbDkConfigurationLayout layout;
    for (unsigned value : {1u,3u,4u,255u})
    {
        auto bytes = configuration(static_cast<unsigned char>(value));
        assert(UsbDkParseConfiguration(bytes.data(), bytes.size(), static_cast<unsigned char>(value), layout));
        assert(layout.Count == 2 && layout.InterfaceOffsets[0] == 9 && layout.InterfaceOffsets[1] == 25);
        for (size_t size = 0; size < bytes.size(); ++size)
        {
            assert(!UsbDkParseConfiguration(bytes.data(), size, static_cast<unsigned char>(value), layout));
        }
        assert(!UsbDkParseConfiguration(bytes.data(), bytes.size(), 0, layout));
        assert(!UsbDkParseConfiguration(bytes.data(), bytes.size(), static_cast<unsigned char>(value ^ 0x40), layout));
    }
    for (auto mutation : {0,1,2,4,9,10,13,18,19,20,21,27,29,36})
    {
        auto bytes = configuration(3);
        bytes[mutation] = 0;
        assert(!UsbDkParseConfiguration(bytes.data(), bytes.size(), 3, layout));
    }
    auto bytes = configuration(3);
    unsigned firstMask, secondMask;
    assert(UsbDkInterfaceEndpointMask(bytes.data(), bytes.size(), 9, firstMask));
    assert(UsbDkInterfaceEndpointMask(bytes.data(), bytes.size(), 25, secondMask));
    assert(UsbDkEndpointMasksCompatible(firstMask, secondMask));
    assert(!UsbDkEndpointMasksCompatible(firstMask, firstMask));
    bytes[36] = 0x81; // An alternate selecting an already active endpoint is refused.
    assert(!UsbDkParseConfiguration(bytes.data(), bytes.size(), 3, layout));
    assert(UsbDkInterfaceEndpointMask(bytes.data(), bytes.size(), 25, secondMask));
    assert(!UsbDkEndpointMasksCompatible(firstMask, secondMask));

    int owner, foreign;
    assert(UsbDkConfigurationOwnerMatches(&owner, &owner));
    assert(!UsbDkConfigurationOwnerMatches(nullptr, &owner));
    assert(!UsbDkConfigurationOwnerMatches(&owner, nullptr));
    assert(!UsbDkConfigurationOwnerMatches(&owner, &foreign));

    std::vector<int> order;
    struct Queue
    {
        std::vector<int> &order;
        bool purged = false;
        void Purge() { purged = true; order.push_back(1); }
        void PurgeSynchronously() { assert(purged); order.push_back(3); }
    } queue{order};
    struct Target
    {
        std::vector<int> &order;
        bool callbacksDone = false;
        void Purge() { assert(order.back() == 1); callbacksDone = true; order.push_back(2); }
        void Start() { assert(callbacksDone && order.back() == 3); order.push_back(4); }
    } target{order};
    UsbDkQuiesceConfigurationData(queue, target);
    assert((order == std::vector<int>{1,2,3,4}));
    // A selector may fail after quiescence; there are no sent callbacks or
    // data presentations left whose buffers could refer to old pipe handles.
    assert(target.callbacksDone && queue.purged);

    size_t actual;
    assert(UsbDkActualTransferLength(true,true,12,16,actual) && actual == 12);
    assert(!UsbDkActualTransferLength(true,true,17,16,actual) && actual == 0);
    assert(UsbDkActualTransferLength(false,true,16,16,actual) && actual == 0);
    assert(UsbDkActualTransferLength(true,false,16,16,actual) && actual == 0);
    assert(UsbDkActualTransferLength(false,false,100,16,actual) && actual == 0);
    puts("configuration descriptor, ownership, drain ordering, alternate collision and failed-transfer bounds: PASS");
}

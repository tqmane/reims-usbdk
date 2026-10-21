// SPDX-License-Identifier: Apache-2.0
#pragma once

template<class TQueue, class TTarget>
void UsbDkQuiesceConfigurationData(TQueue &Queue, TTarget &Target)
{
    Queue.Purge();
    Target.Purge();
    Queue.PurgeSynchronously();
    Target.Start();
}

inline bool UsbDkConfigurationOwnerMatches(const void *Owner, const void *File)
{
    return Owner != nullptr && Owner == File;
}

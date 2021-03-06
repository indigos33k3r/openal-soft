/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "backends/solaris.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <math.h>

#include <thread>
#include <functional>

#include "alMain.h"
#include "alu.h"
#include "alconfig.h"
#include "threads.h"
#include "vector.h"
#include "compat.h"

#include <sys/audioio.h>


namespace {

constexpr ALCchar solaris_device[] = "Solaris Default";

const char *solaris_driver = "/dev/audio";


struct SolarisBackend final : public BackendBase {
    SolarisBackend(ALCdevice *device) noexcept : BackendBase{device} { }
    ~SolarisBackend() override;

    int mixerProc();

    ALCenum open(const ALCchar *name) override;
    ALCboolean reset() override;
    ALCboolean start() override;
    void stop() override;

    int mFd{-1};

    al::vector<ALubyte> mBuffer;

    std::atomic<bool> mKillNow{true};
    std::thread mThread;

    static constexpr inline const char *CurrentPrefix() noexcept { return "SolarisBackend::"; }
    DEF_NEWDEL(SolarisBackend)
};

SolarisBackend::~SolarisBackend()
{
    if(mFd != -1)
        close(mFd);
    mFd = -1;
}

int SolarisBackend::mixerProc()
{
    SetRTPriority();
    althrd_setname(MIXER_THREAD_NAME);

    const int frame_size{mDevice->frameSizeFromFmt()};

    lock();
    while(!mKillNow.load(std::memory_order_acquire) &&
          mDevice->Connected.load(std::memory_order_acquire))
    {
        pollfd pollitem{};
        pollitem.fd = mFd;
        pollitem.events = POLLOUT;

        unlock();
        int pret{poll(&pollitem, 1, 1000)};
        lock();
        if(pret < 0)
        {
            if(errno == EINTR || errno == EAGAIN)
                continue;
            ERR("poll failed: %s\n", strerror(errno));
            aluHandleDisconnect(mDevice, "Failed to wait for playback buffer: %s",
                strerror(errno));
            break;
        }
        else if(pret == 0)
        {
            WARN("poll timeout\n");
            continue;
        }

        ALubyte *write_ptr{mBuffer.data()};
        size_t to_write{mBuffer.size()};
        aluMixData(mDevice, write_ptr, to_write/frame_size);
        while(to_write > 0 && !mKillNow.load(std::memory_order_acquire))
        {
            ssize_t wrote{write(mFd, write_ptr, to_write)};
            if(wrote < 0)
            {
                if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                ERR("write failed: %s\n", strerror(errno));
                aluHandleDisconnect(mDevice, "Failed to write playback samples: %s",
                    strerror(errno));
                break;
            }

            to_write -= wrote;
            write_ptr += wrote;
        }
    }
    unlock();

    return 0;
}


ALCenum SolarisBackend::open(const ALCchar *name)
{
    if(!name)
        name = solaris_device;
    else if(strcmp(name, solaris_device) != 0)
        return ALC_INVALID_VALUE;

    mFd = ::open(solaris_driver, O_WRONLY);
    if(mFd == -1)
    {
        ERR("Could not open %s: %s\n", solaris_driver, strerror(errno));
        return ALC_INVALID_VALUE;
    }

    mDevice->DeviceName = name;
    return ALC_NO_ERROR;
}

ALCboolean SolarisBackend::reset()
{
    audio_info_t info;
    AUDIO_INITINFO(&info);

    info.play.sample_rate = mDevice->Frequency;

    if(mDevice->FmtChans != DevFmtMono)
        mDevice->FmtChans = DevFmtStereo;
    ALsizei numChannels{mDevice->channelsFromFmt()};
    info.play.channels = numChannels;

    switch(mDevice->FmtType)
    {
        case DevFmtByte:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
        case DevFmtUByte:
            info.play.precision = 8;
            info.play.encoding = AUDIO_ENCODING_LINEAR8;
            break;
        case DevFmtUShort:
        case DevFmtInt:
        case DevFmtUInt:
        case DevFmtFloat:
            mDevice->FmtType = DevFmtShort;
            /* fall-through */
        case DevFmtShort:
            info.play.precision = 16;
            info.play.encoding = AUDIO_ENCODING_LINEAR;
            break;
    }

    ALsizei frameSize{numChannels * mDevice->bytesFromFmt()};
    info.play.buffer_size = mDevice->UpdateSize*mDevice->NumUpdates * frameSize;

    if(ioctl(mFd, AUDIO_SETINFO, &info) < 0)
    {
        ERR("ioctl failed: %s\n", strerror(errno));
        return ALC_FALSE;
    }

    if(mDevice->channelsFromFmt() != (ALsizei)info.play.channels)
    {
        ERR("Failed to set %s, got %u channels instead\n", DevFmtChannelsString(mDevice->FmtChans),
            info.play.channels);
        return ALC_FALSE;
    }

    if(!((info.play.precision == 8 && info.play.encoding == AUDIO_ENCODING_LINEAR8 && mDevice->FmtType == DevFmtUByte) ||
         (info.play.precision == 8 && info.play.encoding == AUDIO_ENCODING_LINEAR && mDevice->FmtType == DevFmtByte) ||
         (info.play.precision == 16 && info.play.encoding == AUDIO_ENCODING_LINEAR && mDevice->FmtType == DevFmtShort) ||
         (info.play.precision == 32 && info.play.encoding == AUDIO_ENCODING_LINEAR && mDevice->FmtType == DevFmtInt)))
    {
        ERR("Could not set %s samples, got %d (0x%x)\n", DevFmtTypeString(mDevice->FmtType),
            info.play.precision, info.play.encoding);
        return ALC_FALSE;
    }

    mDevice->Frequency = info.play.sample_rate;
    mDevice->UpdateSize = (info.play.buffer_size/mDevice->NumUpdates) + 1;

    SetDefaultChannelOrder(mDevice);

    mBuffer.resize(mDevice->UpdateSize * mDevice->frameSizeFromFmt());
    std::fill(mBuffer.begin(), mBuffer.end(), 0);

    return ALC_TRUE;
}

ALCboolean SolarisBackend::start()
{
    try {
        mKillNow.store(false, std::memory_order_release);
        mThread = std::thread{std::mem_fn(&SolarisBackend::mixerProc), this};
        return ALC_TRUE;
    }
    catch(std::exception& e) {
        ERR("Could not create playback thread: %s\n", e.what());
    }
    catch(...) {
    }
    return ALC_FALSE;
}

void SolarisBackend::stop()
{
    if(mKillNow.exchange(true, std::memory_order_acq_rel) || !mThread.joinable())
        return;
    mThread.join();

    if(ioctl(mFd, AUDIO_DRAIN) < 0)
        ERR("Error draining device: %s\n", strerror(errno));
}

} // namespace

BackendFactory &SolarisBackendFactory::getFactory()
{
    static SolarisBackendFactory factory{};
    return factory;
}

bool SolarisBackendFactory::init()
{
    ConfigValueStr(nullptr, "solaris", "device", &solaris_driver);
    return true;
}

bool SolarisBackendFactory::querySupport(BackendType type)
{ return type == BackendType::Playback; }

void SolarisBackendFactory::probe(DevProbe type, std::string *outnames)
{
    switch(type)
    {
        case ALL_DEVICE_PROBE:
        {
#ifdef HAVE_STAT
            struct stat buf;
            if(stat(solaris_driver, &buf) == 0)
#endif
                outnames->append(solaris_device, sizeof(solaris_device));
        }
        break;

        case CAPTURE_DEVICE_PROBE:
            break;
    }
}

BackendPtr SolarisBackendFactory::createBackend(ALCdevice *device, BackendType type)
{
    if(type == BackendType::Playback)
        return BackendPtr{new SolarisBackend{device}};
    return nullptr;
}

/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "AudioPort"

#include <algorithm>

#include <android-base/stringprintf.h>
#include <media/AudioPort.h>
#include <utils/Log.h>

namespace android {

void AudioPort::importAudioPort(const sp<AudioPort>& port, bool force __unused)
{
    for (const auto& profileToImport : port->mProfiles) {
        // Import only valid port, i.e. valid format, non empty rates and channels masks
        if (!profileToImport->isValid()) {
            continue;
        }
        if (std::find_if(mProfiles.begin(), mProfiles.end(),
                [profileToImport](const auto &profile) {
                        return *profile == *profileToImport; }) == mProfiles.end()) {
            addAudioProfile(profileToImport);
        }
    }
}

void AudioPort::toAudioPort(struct audio_port *port) const {
    // TODO: update this function once audio_port structure reflects the new profile definition.
    // For compatibility reason: flatening the AudioProfile into audio_port structure.
    FormatSet flatenedFormats;
    SampleRateSet flatenedRates;
    ChannelMaskSet flatenedChannels;
    for (const auto& profile : mProfiles) {
        if (profile->isValid()) {
            audio_format_t formatToExport = profile->getFormat();
            const SampleRateSet &ratesToExport = profile->getSampleRates();
            const ChannelMaskSet &channelsToExport = profile->getChannels();

            flatenedFormats.insert(formatToExport);
            flatenedRates.insert(ratesToExport.begin(), ratesToExport.end());
            flatenedChannels.insert(channelsToExport.begin(), channelsToExport.end());

            if (flatenedRates.size() > AUDIO_PORT_MAX_SAMPLING_RATES ||
                    flatenedChannels.size() > AUDIO_PORT_MAX_CHANNEL_MASKS ||
                    flatenedFormats.size() > AUDIO_PORT_MAX_FORMATS) {
                ALOGE("%s: bailing out: cannot export profiles to port config", __func__);
                return;
            }
        }
    }
    port->role = mRole;
    port->type = mType;
    strlcpy(port->name, mName.c_str(), AUDIO_PORT_MAX_NAME_LEN);
    port->num_sample_rates = flatenedRates.size();
    port->num_channel_masks = flatenedChannels.size();
    port->num_formats = flatenedFormats.size();
    std::copy(flatenedRates.begin(), flatenedRates.end(), port->sample_rates);
    std::copy(flatenedChannels.begin(), flatenedChannels.end(), port->channel_masks);
    std::copy(flatenedFormats.begin(), flatenedFormats.end(), port->formats);

    ALOGV("AudioPort::toAudioPort() num gains %zu", mGains.size());

    port->num_gains = std::min(mGains.size(), (size_t) AUDIO_PORT_MAX_GAINS);
    for (size_t i = 0; i < port->num_gains; i++) {
        port->gains[i] = mGains[i]->getGain();
    }
}

void AudioPort::dump(std::string *dst, int spaces, bool verbose) const {
    if (!mName.empty()) {
        dst->append(base::StringPrintf("%*s- name: %s\n", spaces, "", mName.c_str()));
    }
    if (verbose) {
        std::string profilesStr;
        mProfiles.dump(&profilesStr, spaces);
        dst->append(profilesStr);

        if (mGains.size() != 0) {
            dst->append(base::StringPrintf("%*s- gains:\n", spaces, ""));
            for (size_t i = 0; i < mGains.size(); i++) {
                std::string gainStr;
                mGains[i]->dump(&gainStr, spaces + 2, i);
                dst->append(gainStr);
            }
        }
    }
}

void AudioPort::log(const char* indent) const
{
    ALOGI("%s Port[nm:%s, type:%d, role:%d]", indent, mName.c_str(), mType, mRole);
}

// --- AudioPortConfig class implementation

status_t AudioPortConfig::applyAudioPortConfig(
        const struct audio_port_config *config,
        struct audio_port_config *backupConfig __unused)
{
    if (config->config_mask & AUDIO_PORT_CONFIG_SAMPLE_RATE) {
        mSamplingRate = config->sample_rate;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_CHANNEL_MASK) {
        mChannelMask = config->channel_mask;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_FORMAT) {
        mFormat = config->format;
    }
    if (config->config_mask & AUDIO_PORT_CONFIG_GAIN) {
        mGain = config->gain;
    }

    return NO_ERROR;
}

namespace {

template<typename T>
void updateField(
        const T& portConfigField, T audio_port_config::*port_config_field,
        struct audio_port_config *dstConfig, const struct audio_port_config *srcConfig,
        unsigned int configMask, T defaultValue)
{
    if (dstConfig->config_mask & configMask) {
        if ((srcConfig != nullptr) && (srcConfig->config_mask & configMask)) {
            dstConfig->*port_config_field = srcConfig->*port_config_field;
        } else {
            dstConfig->*port_config_field = portConfigField;
        }
    } else {
        dstConfig->*port_config_field = defaultValue;
    }
}

} // namespace

void AudioPortConfig::toAudioPortConfig(
        struct audio_port_config *dstConfig,
        const struct audio_port_config *srcConfig) const
{
    updateField(mSamplingRate, &audio_port_config::sample_rate,
            dstConfig, srcConfig, AUDIO_PORT_CONFIG_SAMPLE_RATE, 0u);
    updateField(mChannelMask, &audio_port_config::channel_mask,
            dstConfig, srcConfig, AUDIO_PORT_CONFIG_CHANNEL_MASK,
            (audio_channel_mask_t)AUDIO_CHANNEL_NONE);
    updateField(mFormat, &audio_port_config::format,
            dstConfig, srcConfig, AUDIO_PORT_CONFIG_FORMAT, AUDIO_FORMAT_INVALID);
    dstConfig->id = mId;

    sp<AudioPort> audioport = getAudioPort();
    if ((dstConfig->config_mask & AUDIO_PORT_CONFIG_GAIN) && audioport != NULL) {
        dstConfig->gain = mGain;
        if ((srcConfig != NULL) && (srcConfig->config_mask & AUDIO_PORT_CONFIG_GAIN)
                && audioport->checkGain(&srcConfig->gain, srcConfig->gain.index) == OK) {
            dstConfig->gain = srcConfig->gain;
        }
    } else {
        dstConfig->gain.index = -1;
    }
    if (dstConfig->gain.index != -1) {
        dstConfig->config_mask |= AUDIO_PORT_CONFIG_GAIN;
    } else {
        dstConfig->config_mask &= ~AUDIO_PORT_CONFIG_GAIN;
    }
}

bool AudioPortConfig::hasGainController(bool canUseForVolume) const
{
    sp<AudioPort> audioport = getAudioPort();
    if (!audioport) {
        return false;
    }
    return canUseForVolume ? audioport->getGains().canUseForVolume()
                           : audioport->getGains().size() > 0;
}

}

#include "bronze_host/host_audio_internal.h"
#include "util/asset_path.h"

namespace bro::bronze_host {

void registerAudioContextClips(ObjectBuilder& b) {
    b.def("createClip", 3, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);

        Value first = a[0];
        if (auto* hostBuf = hostAudioBufferOf(first)) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            if (frames <= 0 || channels <= 0) return ev::fromDouble(-1);

            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(first, key);
                if (ev::isTypedArray(arr)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
                    if (info && info.data) {
                        size_t count = std::min(static_cast<size_t>(frames), static_cast<size_t>(info.elementCount));
                        std::memcpy(chData[c].data(), info.data, count * sizeof(float));
                    }
                } else if (c < static_cast<int>(hostBuf->channels.size())) {
                    chData[c] = hostBuf->channels[c];
                }
            }

            std::vector<float> interleaved(frames * channels);
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    interleaved[f * channels + c] = chData[c][f];
                }
            }

            int clipId = e->createClip(interleaved.data(), frames * channels, channels);
            return ev::fromDouble(clipId);
        }

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(first, &rawData, &rawLen, &elemSize) || rawLen == 0) {
            return ev::throwTypeError("createClip: expected AudioBuffer or Float32Array");
        }

        int numSamples = static_cast<int>(rawLen / sizeof(float));
        int channels = a.size() >= 2 ? i32At(a, 1) : 1;
        if (channels <= 0) channels = 1;

        const float* samples = reinterpret_cast<const float*>(rawData);
        std::vector<float> resampled;
        if (a.size() >= 3 && !ev::isUndefined(a[2])) {
            int srcRate = i32At(a, 2);
            int engRate = e->sampleRate();
            if (srcRate > 0 && srcRate != engRate && channels > 0) {
                resampled = broaudio::resample(samples, numSamples / channels, channels, srcRate, engRate);
                if (!resampled.empty()) {
                    samples = resampled.data();
                    numSamples = static_cast<int>(resampled.size());
                }
            }
        }

        int clipId = e->createClip(samples, numSamples, channels);
        return ev::fromDouble(clipId);
    });

    b.def("deleteClip", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->deleteClip(i32At(a, 0));
        return ev::undefined();
    });

    b.def("getClipSampleCount", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getClipSampleCount(i32At(a, 0)) : 0);
    });

    b.def("getClipChannels", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getClipChannels(i32At(a, 0)) : 0);
    });

    b.def("getClipWaveform", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.size() < 2) return ev::null();
        int clipId = i32At(a, 0);
        int numBins = i32At(a, 1);
        if (numBins <= 0 || numBins > 1024) return ev::null();

        auto wf = e->getClipWaveform(clipId, numBins);
        if (wf.empty()) return ev::null();

        Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(wf.size()));
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wf.data()), wf.size() * sizeof(float)));
        return arr;
    });

    b.def("createClipFromFile", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetPath(rawPath);
        int clipId = e->createClipFromFile(path.c_str());
        return ev::fromDouble(clipId);
    });

    b.def("createClipFromFileAsync", 1, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent p{ev::createPromise()};
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) {
            ev::rejectPromise(p.get(), hostMakeDomError("Error", "createClipFromFileAsync: no audio engine or path"));
            return p.get();
        }
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetPath(rawPath);
        std::string err;
        int clipId = e->createClipFromFileEx(path.c_str(), &err);
        if (clipId >= 0) {
            ev::resolvePromise(p.get(), ev::fromDouble(clipId));
        } else {
            std::string msg = err.empty() ? "createClipFromFileAsync failed" : err;
            ev::rejectPromise(p.get(), hostMakeDomError("Error", msg));
        }
        return p.get();
    });

    b.def("decodeAudioData", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        Value inputV = a[0];
        Value successCb = a.size() >= 2 ? a[1] : ev::undefined();
        Value errorCb = a.size() >= 3 ? a[2] : ev::undefined();

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(inputV, &rawData, &rawLen, &elemSize) || rawLen == 0) {
            if (ev::isFunction(errorCb)) {
                Value err = hostMakeDomError("EncodingError", "decodeAudioData: invalid buffer");
                ev::call(errorCb, ev::undefined(), std::span<const Value>(&err, 1));
            }
            return ev::null();
        }

        broaudio::AudioFileData data = broaudio::loadAudioFileFromMemory(rawData, rawLen);
        if (!data.valid()) {
            if (ev::isFunction(errorCb)) {
                std::string msg = data.error.empty() ? "decodeAudioData: failed to decode audio" : data.error;
                Value err = hostMakeDomError("EncodingError", msg);
                ev::call(errorCb, ev::undefined(), std::span<const Value>(&err, 1));
            }
            return ev::null();
        }

        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        int engRate = e ? e->sampleRate() : 44100;
        std::vector<float> samples;
        int numFrames = data.numFrames;
        if (data.sampleRate > 0 && data.sampleRate != engRate && data.channels > 0) {
            samples = broaudio::resample(data.samples.data(), data.numFrames, data.channels, data.sampleRate, engRate);
            numFrames = static_cast<int>(samples.size() / data.channels);
        } else {
            samples = std::move(data.samples);
        }

        ObjectBuilder res;
        Value samplesArr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(samples.size()));
        ev::fillTypedArray(samplesArr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(float)));
        res.set("samples", samplesArr);
        res.set("channels", ev::fromDouble(data.channels));
        res.set("sampleRate", ev::fromDouble(engRate));
        res.set("numFrames", ev::fromDouble(numFrames));
        res.set("numberOfChannels", ev::fromDouble(data.channels));
        res.set("length", ev::fromDouble(numFrames));
        res.set("duration", ev::fromDouble(static_cast<double>(numFrames) / engRate));

        int chCount = data.channels;
        res.def("getChannelData", 1, [samples, chCount, numFrames](Value, std::span<const Value> ca) -> Value {
            int c = ca.empty() ? 0 : i32At(ca, 0);
            if (c < 0 || c >= chCount || numFrames <= 0) return ev::null();
            std::vector<float> ch(numFrames);
            for (int i = 0; i < numFrames; ++i) ch[i] = samples[i * chCount + c];
            Value out = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(numFrames));
            ev::fillTypedArray(out, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(ch.data()), numFrames * sizeof(float)));
            return out;
        });

        res.def("copyFromChannel", 3, [samples, chCount, numFrames](Value, std::span<const Value> ca) -> Value {
            if (ca.size() < 2) return ev::undefined();
            Value dest = ca[0];
            int c = i32At(ca, 1);
            int offset = ca.size() >= 3 ? i32At(ca, 2) : 0;
            if (c < 0 || c >= chCount || offset < 0 || offset >= numFrames) return ev::undefined();
            if (ev::isTypedArray(dest)) {
                ev::TypedArrayInfo info = ev::typedArrayInfo(dest);
                if (info && info.data) {
                    float* destPtr = reinterpret_cast<float*>(info.data);
                    int count = std::min(static_cast<int>(info.elementCount), numFrames - offset);
                    for (int i = 0; i < count; ++i) destPtr[i] = samples[(offset + i) * chCount + c];
                }
            }
            return ev::undefined();
        });

        res.def("then", 2, [](Value self, std::span<const Value> ta) -> Value {
            if (!ta.empty() && ev::isFunction(ta[0])) {
                try {
                    ev::call(ta[0], ev::undefined(), std::span<const Value>(&self, 1));
                } catch (...) {}
            }
            return self;
        });

        res.def("catch", 1, [](Value self, std::span<const Value>) -> Value {
            return self;
        });

        Value resVal = res.get();
        if (ev::isFunction(successCb)) {
            try {
                ev::call(successCb, ev::undefined(), std::span<const Value>(&resVal, 1));
            } catch (...) {}
        }
        return resVal;
    });

    b.def("decodeAudioFile", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::null();
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetPath(rawPath);
        broaudio::AudioFileData data = broaudio::loadAudioFile(path.c_str());
        if (!data.valid()) return ev::null();

        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        int engRate = e ? e->sampleRate() : 44100;
        std::vector<float> samples;
        int numFrames = data.numFrames;
        if (data.sampleRate > 0 && data.sampleRate != engRate && data.channels > 0) {
            samples = broaudio::resample(data.samples.data(), data.numFrames, data.channels, data.sampleRate, engRate);
            numFrames = static_cast<int>(samples.size() / data.channels);
        } else {
            samples = std::move(data.samples);
        }

        ObjectBuilder res;
        Value samplesArr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(samples.size()));
        ev::fillTypedArray(samplesArr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(samples.data()), samples.size() * sizeof(float)));
        res.set("samples", samplesArr);
        res.set("channels", ev::fromDouble(data.channels));
        res.set("sampleRate", ev::fromDouble(engRate));
        res.set("numFrames", ev::fromDouble(numFrames));
        res.set("numberOfChannels", ev::fromDouble(data.channels));
        res.set("length", ev::fromDouble(numFrames));
        res.set("duration", ev::fromDouble(static_cast<double>(numFrames) / engRate));
        return res.get();
    });

    b.def("saveWav", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::fromBool(false);
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetWritePath(rawPath);

        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(a[1], &rawData, &rawLen, &elemSize) || rawLen == 0) {
            return ev::fromBool(false);
        }
        int channels = i32At(a, 2);
        int sampleRate = i32At(a, 3);
        if (channels <= 0 || sampleRate <= 0) return ev::fromBool(false);
        int numFrames = static_cast<int>(rawLen / (channels * sizeof(float)));
        const float* samples = reinterpret_cast<const float*>(rawData);
        bool ok = broaudio::saveWav(path.c_str(), samples, numFrames, channels, sampleRate);
        return ev::fromBool(ok);
    });

    b.def("exportRecordingToWav", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromBool(false);
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetWritePath(rawPath);
        return ev::fromBool(e->exportRecordingToWav(path.c_str()));
    });

    b.def("startRecording", 0, [](Value, std::span<const Value>) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e) e->startRecording();
        return ev::undefined();
    });

    b.def("stopRecording", 0, [](Value, std::span<const Value>) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e) return ev::null();
        e->stopRecording();
        std::vector<float> rec = e->getRecordBuffer();
        if (rec.empty()) return ev::null();
        Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(rec.size()));
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(rec.data()), rec.size() * sizeof(float)));
        return arr;
    });

    b.def("playClip", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::fromDouble(-1);
        int clipId = i32At(a, 0);
        float gain = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        bool loop = false;
        float pan = 0.0f;

        if (a.size() >= 3) {
            if (ev::isBool(a[2])) {
                loop = boolAt(a, 2);
            } else {
                pan = static_cast<float>(numAt(a, 2));
                if (a.size() >= 4) loop = boolAt(a, 3);
            }
        }

        int playbackId = e->playClip(clipId, gain, loop);
        if (pan != 0.0f && playbackId >= 0) {
            e->setPlaybackPan(playbackId, pan);
        }
        return ev::fromDouble(playbackId);
    });

    b.def("stopPlayback", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->stopPlayback(i32At(a, 0));
        return ev::undefined();
    });

    b.def("setPlaybackGain", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackGain(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackLoop", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackLoop(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackPlaying", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackPlaying(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackRegion", 3, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 3) e->setPlaybackRegion(i32At(a, 0), static_cast<int>(numAt(a, 1)), static_cast<int>(numAt(a, 2)));
        return ev::undefined();
    });

    b.def("setPlaybackRate", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackRate(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackPan", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackPan(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("getPlaybackPosition", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackPosition(i32At(a, 0)) : 0);
    });

    b.def("getPlaybackPositionSeconds", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackPositionSeconds(i32At(a, 0)) : 0.0);
    });

    b.def("seekPlayback", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->seekPlayback(i32At(a, 0), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialEnabled", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackSpatialEnabled(i32At(a, 0), boolAt(a, 1));
        return ev::undefined();
    });

    b.def("setPlaybackSpatialPosition", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 4) {
            e->setPlaybackSpatialPosition(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("setPlaybackSpatialVelocity", 4, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 4) {
            e->setPlaybackSpatialVelocity(i32At(a, 0), static_cast<float>(numAt(a, 1)),
                                          static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        }
        return ev::undefined();
    });

    b.def("getPlaybackDopplerRatio", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        return ev::fromDouble(e && !a.empty() ? e->getPlaybackDopplerRatio(i32At(a, 0)) : 1.0);
    });

    b.def("setPlaybackBus", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && a.size() >= 2) e->setPlaybackBus(i32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });

    b.def("createStream", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e) return ev::fromDouble(-1);
        int channels = a.size() >= 1 ? i32At(a, 0) : 1;
        int ringFrames = a.size() >= 2 ? i32At(a, 1) : 44100;
        int streamId = e->createStream(channels, ringFrames);
        return ev::fromDouble(streamId);
    });

    b.def("pushStreamSamples", 2, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.size() < 2) return ev::fromDouble(0);
        int id = i32At(a, 0);
        const uint8_t* rawData = nullptr;
        size_t rawLen = 0;
        size_t elemSize = 1;
        if (!bufferBytes(a[1], &rawData, &rawLen, &elemSize) || rawLen == 0) return ev::fromDouble(0);
        int count = static_cast<int>(rawLen / sizeof(float));
        int pushed = e->pushStreamSamples(id, reinterpret_cast<const float*>(rawData), count);
        return ev::fromDouble(pushed);
    });

    b.def("closeStream", 1, [](Value, std::span<const Value> a) {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (e && !a.empty()) e->closeStream(i32At(a, 0));
        return ev::undefined();
    });

    b.def("createStreamFromFile", 2, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::throwError("createStreamFromFile: no engine or path");
        std::string rawPath = ev::toUtf8(a[0]);
        std::string path = bro::util::resolveAssetPath(rawPath);
        broaudio::FileStreamOptions opts;
        if (a.size() >= 2 && a[1].isObject()) {
            Value optVal = a[1];
            Value rf = ev::getProperty(optVal, "ringFrames");
            if (ev::isNumber(rf)) opts.ringFrames = static_cast<int>(ev::toDouble(rf));
            Value loop = ev::getProperty(optVal, "loop");
            if (ev::isBool(loop)) opts.loop = ev::toBool(loop);
        }
        std::string err;
        int id = e->createStreamFromFile(path.c_str(), opts, &err);
        if (id < 0) {
            std::string msg = err.empty() ? ("createStreamFromFile: failed to open " + rawPath) : err;
            return ev::throwError(msg.c_str());
        }
        return ev::fromDouble(id);
    });

    b.def("getStreamStats", 1, [](Value, std::span<const Value> a) -> Value {
        auto* e = hostEngine() ? hostEngine()->audioEngine() : nullptr;
        if (!e || a.empty()) return ev::null();
        broaudio::StreamStats stats = e->getStreamStats(i32At(a, 0));
        if (!stats.valid) return ev::null();

        ObjectBuilder obj;
        obj.set("decodedFrames", ev::fromDouble(stats.decodedFrames));
        obj.set("playedFrames", ev::fromDouble(stats.playedFrames));
        obj.set("bufferedFrames", ev::fromDouble(stats.bufferedFrames));
        obj.set("underrunFrames", ev::fromDouble(stats.underrunFrames));
        obj.set("finished", ev::fromBool(stats.finished));
        return obj.get();
    });
}

}  // namespace bro::bronze_host

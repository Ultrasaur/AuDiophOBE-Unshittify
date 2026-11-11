#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION

#include "dr_wav.h"
#include "dr_mp3.h"
#include "dr_flac.h"

#include "audio.h"

#include <FLAC/stream_encoder.h>
#include <lame/lame.h>

#include <algorithm>
#include <limits>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <vector>
#include <new>
#include <stdexcept>

using namespace std;
namespace fs = std::filesystem;

// Проверка размеров аудиоданных
bool validateAudioSize(
    uint64_t frameCount,
    unsigned int channels,
    size_t& sampleCount)
{
    if (frameCount == 0 || channels == 0)
    {
        return false;
    }

    if (frameCount >
        numeric_limits<size_t>::max() / channels)
    {
        return false;
    }

    sampleCount =
        static_cast<size_t>(frameCount) * channels;

    return true;
}

// Проверка аудиоданных перед сохранением
bool validateAudio(
    const AudioData& audio,
    string& error)
{
    if (audio.channels == 0 ||
        audio.sampleRate == 0 ||
        audio.frameCount == 0)
    {
        error = "Invalid audio parameters.";
        return false;
    }

    size_t sampleCount = 0;

    if (!validateAudioSize(
        audio.frameCount,
        audio.channels,
        sampleCount))
    {
        error = "Invalid audio dimensions.";
        return false;
    }

    if (audio.samples.size() != sampleCount)
    {
        error = "Audio data size does not match frame count.";
        return false;
    }

    return true;
}

// Загрузка WAV
bool loadWav(
    const fs::path& filename,
    AudioData& audio,
    string& error)
{
    unsigned int channels = 0;
    unsigned int sampleRate = 0;
    drwav_uint64 frameCount = 0;

    float* data = drwav_open_file_and_read_pcm_frames_f32_w(
        filename.c_str(),
        &channels,
        &sampleRate,
        &frameCount,
        nullptr
    );

    if (data == nullptr)
    {
        error = "Unable to open or read WAV file.";
        return false;
    }

    size_t sampleCount = 0;

    if (!validateAudioSize(
        frameCount,
        channels,
        sampleCount))
    {
        drwav_free(data, nullptr);

        error = "Invalid WAV audio dimensions.";
        return false;
    }

    audio.channels = channels;
    audio.sampleRate = sampleRate;
    audio.frameCount = frameCount;

    audio.samples.assign(
        data,
        data + sampleCount
    );

    drwav_free(data, nullptr);

    return true;
}

// Загрузка MP3 потоковыми блоками
bool loadMp3(
    const fs::path& filename,
    AudioData& audio,
    string& error)
{
    audio = {};

    drmp3 decoder = {};

    if (!drmp3_init_file_w(
        &decoder,
        filename.c_str(),
        nullptr))
    {
        error = "Unable to open MP3 file.";
        return false;
    }

    const unsigned int channels = decoder.channels;
    const unsigned int sampleRate = decoder.sampleRate;

    if (channels == 0 || sampleRate == 0)
    {
        drmp3_uninit(&decoder);
        error = "Invalid MP3 format parameters.";
        return false;
    }

    // Не используем drmp3_get_pcm_frame_count() как обязательный
    // размер выходного массива: для некоторых MP3 предварительная
    // оценка количества кадров может отличаться от фактической.
    constexpr drmp3_uint64 chunkFrames = 16384;

    size_t chunkSampleCount = 0;

    if (!validateAudioSize(
        chunkFrames,
        channels,
        chunkSampleCount))
    {
        drmp3_uninit(&decoder);
        error = "Invalid MP3 decoding buffer dimensions.";
        return false;
    }

    try
    {
        vector<float> chunk(chunkSampleCount);

        uint64_t totalFrames = 0;
        bool allocationFailed = false;

        while (true)
        {
            drmp3_uint64 framesRead =
                drmp3_read_pcm_frames_f32(
                    &decoder,
                    chunkFrames,
                    chunk.data()
                );

            if (framesRead == 0)
            {
                // Нулевое количество кадров означает конец
                // доступного декодированного потока.
                break;
            }

            if (framesRead > chunkFrames)
            {
                error = "MP3 decoder returned an invalid frame count.";
                allocationFailed = true;
                break;
            }

            if (totalFrames >
                numeric_limits<uint64_t>::max() - framesRead)
            {
                error = "MP3 frame count overflow.";
                allocationFailed = true;
                break;
            }

            size_t framesAsSize = static_cast<size_t>(framesRead);

            if (framesAsSize >
                numeric_limits<size_t>::max() / channels)
            {
                error = "MP3 sample count overflow.";
                allocationFailed = true;
                break;
            }

            size_t samplesRead = framesAsSize * channels;

            if (samplesRead > audio.samples.max_size() -
                audio.samples.size())
            {
                error = "Decoded MP3 is too large for memory.";
                allocationFailed = true;
                break;
            }

            audio.samples.insert(
                audio.samples.end(),
                chunk.begin(),
                chunk.begin() + samplesRead
            );

            totalFrames += framesRead;
        }

        drmp3_uninit(&decoder);

        if (allocationFailed)
        {
            audio = {};
            return false;
        }

        if (totalFrames == 0)
        {
            audio = {};
            error = "MP3 file contains no decodable audio frames.";
            return false;
        }

        audio.channels = channels;
        audio.sampleRate = sampleRate;
        audio.frameCount = totalFrames;

        return true;
    }
    catch (const bad_alloc&)
    {
        drmp3_uninit(&decoder);
        audio = {};
        error = "Not enough memory to decode MP3 file.";
        return false;
    }
    catch (const length_error&)
    {
        drmp3_uninit(&decoder);
        audio = {};
        error = "Decoded MP3 exceeds the supported memory size.";
        return false;
    }
}

// Загрузка FLAC
bool loadFlac(
    const fs::path& filename,
    AudioData& audio,
    string& error)
{
    drflac* decoder = drflac_open_file_w(
        filename.c_str(),
        nullptr
    );

    if (decoder == nullptr)
    {
        error = "Unable to open FLAC file.";
        return false;
    }

    drflac_uint64 frameCount =
        decoder->totalPCMFrameCount;

    size_t sampleCount = 0;

    if (!validateAudioSize(
        frameCount,
        decoder->channels,
        sampleCount))
    {
        drflac_close(decoder);

        error = "Invalid FLAC audio dimensions.";
        return false;
    }

    audio.channels = decoder->channels;
    audio.sampleRate = decoder->sampleRate;
    audio.frameCount = frameCount;

    audio.samples.resize(sampleCount);

    drflac_uint64 framesRead =
        drflac_read_pcm_frames_f32(
            decoder,
            frameCount,
            audio.samples.data()
        );

    drflac_close(decoder);

    if (framesRead != frameCount)
    {
        audio.samples.clear();

        error = "Unable to decode all FLAC frames.";
        return false;
    }

    return true;
}

// Универсальная загрузка
bool loadAudio(
    const fs::path& filename,
    AudioData& audio,
    string& error)
{
    wstring extension = filename.extension().wstring();

    transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        towlower
    );

    if (extension == L".wav")
    {
        return loadWav(filename, audio, error);
    }

    if (extension == L".mp3")
    {
        return loadMp3(filename, audio, error);
    }

    if (extension == L".flac")
    {
        return loadFlac(filename, audio, error);
    }

    error = "Unsupported audio format.";
    return false;
}

// Сохранение WAV с поддержкой Unicode-путей
bool saveWavPath(
    const fs::path& filename,
    const AudioData& audio,
    string& error)
{
    if (!validateAudio(audio, error))
    {
        return false;
    }

    drwav_data_format format = {};

    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = audio.channels;
    format.sampleRate = audio.sampleRate;
    format.bitsPerSample = 32;

    drwav wav;

    if (!drwav_init_file_write_w(
        &wav,
        filename.c_str(),
        &format,
        nullptr))
    {
        error = "Unable to create output WAV file.";
        return false;
    }

    drwav_uint64 writtenFrames =
        drwav_write_pcm_frames(
            &wav,
            audio.frameCount,
            audio.samples.data()
        );

    drwav_uninit(&wav);

    if (writtenFrames != audio.frameCount)
    {
        error = "Unable to write all WAV frames.";
        return false;
    }

    return true;
}

// Сохранение WAV через старый интерфейс
bool saveWav(
    const string& filename,
    const AudioData& audio,
    string& error)
{
    return saveWavPath(fs::path(filename), audio, error);
}

// Сохранение FLAC
bool saveFlac(
    const fs::path& filename,
    const AudioData& audio,
    string& error)
{
    if (!validateAudio(audio, error))
    {
        return false;
    }

    if (audio.channels > 8)
    {
        error = "FLAC supports up to 8 channels.";
        return false;
    }

    const unsigned int bitsPerSample = 24;

    FLAC__StreamEncoder* encoder =
        FLAC__stream_encoder_new();

    if (encoder == nullptr)
    {
        error = "Unable to create FLAC encoder.";
        return false;
    }

    bool configured = true;

    configured = (FLAC__stream_encoder_set_channels(
        encoder,
        audio.channels
    ) != false) && configured;

    configured = (FLAC__stream_encoder_set_sample_rate(
        encoder,
        audio.sampleRate
    ) != false) && configured;

    configured = (FLAC__stream_encoder_set_bits_per_sample(
        encoder,
        bitsPerSample
    ) != false) && configured;

    configured = (FLAC__stream_encoder_set_compression_level(
        encoder,
        8
    ) != false) && configured;

    configured = (FLAC__stream_encoder_set_total_samples_estimate(
        encoder,
        audio.frameCount
    ) != false) && configured;

    if (!configured)
    {
        FLAC__stream_encoder_delete(encoder);

        error = "Unable to configure FLAC encoder.";
        return false;
    }

    auto u8Filename = filename.u8string();
    string outputFilename(
        reinterpret_cast<const char*>(u8Filename.data()),
        u8Filename.size()
    );

    FLAC__StreamEncoderInitStatus initStatus =
        FLAC__stream_encoder_init_file(
            encoder,
            outputFilename.c_str(),
            nullptr,
            nullptr
        );

    if (initStatus != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
    {
        FLAC__stream_encoder_delete(encoder);

        error = "Unable to create output FLAC file.";
        return false;
    }

    const size_t chunkFrames = 4096;

    vector<FLAC__int32> pcm(
        chunkFrames * audio.channels
    );

    bool success = true;

    uint64_t processedFrames = 0;

    while (processedFrames < audio.frameCount)
    {
        size_t currentFrames = static_cast<size_t>(
            min(
                static_cast<uint64_t>(chunkFrames),
                audio.frameCount - processedFrames
            )
            );

        size_t sampleOffset =
            static_cast<size_t>(processedFrames) *
            audio.channels;

        size_t currentSamples =
            currentFrames * audio.channels;

        for (size_t i = 0; i < currentSamples; i++)
        {
            float value = audio.samples[sampleOffset + i];

            if (!isfinite(value))
            {
                success = false;
                error = "Audio contains invalid sample values.";
                break;
            }

            value = max(-1.0f, min(1.0f, value));

            pcm[i] = static_cast<FLAC__int32>(
                lround(value * 8388607.0)
                );
        }

        if (!success)
        {
            break;
        }

        if (!FLAC__stream_encoder_process_interleaved(
            encoder,
            pcm.data(),
            static_cast<unsigned int>(currentFrames)))
        {
            success = false;
            error = "FLAC encoding failed.";
            break;
        }

        processedFrames += currentFrames;
    }

    bool finished =
        FLAC__stream_encoder_finish(encoder);

    FLAC__stream_encoder_delete(encoder);

    if (!success)
    {
        return false;
    }

    if (!finished)
    {
        error = "Unable to finish FLAC encoding.";
        return false;
    }

    return true;
}

// Сохранение MP3
bool saveMp3(
    const fs::path& filename,
    const AudioData& audio,
    string& error)
{
    if (!validateAudio(audio, error))
    {
        return false;
    }

    if (audio.channels > 2)
    {
        error = "MP3 supports only mono or stereo audio.";
        return false;
    }

    if (audio.sampleRate < 8000 ||
        audio.sampleRate > 48000)
    {
        error = "Unsupported MP3 sample rate.";
        return false;
    }

    lame_global_flags* encoder = lame_init();

    if (encoder == nullptr)
    {
        error = "Unable to create MP3 encoder.";
        return false;
    }

    lame_set_num_channels(
        encoder,
        audio.channels
    );

    lame_set_in_samplerate(
        encoder,
        audio.sampleRate
    );

    // Наивысшее качество VBR
    lame_set_VBR(
        encoder,
        vbr_default
    );

    lame_set_VBR_quality(
        encoder,
        0.0f
    );

    // Наивысшее качество алгоритма кодирования
    lame_set_quality(
        encoder,
        0
    );

    lame_set_bWriteVbrTag(encoder, 1);

    if (lame_init_params(encoder) < 0)
    {
        lame_close(encoder);

        error = "Unable to initialize MP3 encoder.";
        return false;
    }

    FILE* outputFile = nullptr;

#ifdef _WIN32
    if (_wfopen_s(
        &outputFile,
        filename.c_str(),
        L"wb") != 0)
    {
        outputFile = nullptr;
    }
#else
    outputFile = fopen(
        filename.string().c_str(),
        "wb"
    );
#endif

    if (outputFile == nullptr)
    {
        lame_close(encoder);

        error = "Unable to create output MP3 file.";
        return false;
    }

    const size_t chunkFrames = 8192;

    vector<short> left(chunkFrames);
    vector<short> right(chunkFrames);

    vector<unsigned char> mp3Buffer(32768);

    uint64_t processedFrames = 0;

    bool success = true;

    while (processedFrames < audio.frameCount)
    {
        size_t currentFrames = static_cast<size_t>(
            min(
                static_cast<uint64_t>(chunkFrames),
                audio.frameCount - processedFrames
            )
            );

        size_t sampleOffset =
            static_cast<size_t>(processedFrames) *
            audio.channels;

        for (size_t i = 0; i < currentFrames; i++)
        {
            float leftSample =
                audio.samples[
                    sampleOffset + i * audio.channels
                ];

            float rightSample = leftSample;

            if (audio.channels == 2)
            {
                rightSample =
                    audio.samples[
                        sampleOffset + i * 2 + 1
                    ];
            }

            if (!isfinite(leftSample) ||
                !isfinite(rightSample))
            {
                success = false;
                error = "Audio contains invalid sample values.";
                break;
            }

            leftSample = max(-1.0f, min(1.0f, leftSample));
            rightSample = max(-1.0f, min(1.0f, rightSample));

            left[i] = static_cast<short>(
                lround(leftSample * 32767.0f)
                );

            right[i] = static_cast<short>(
                lround(rightSample * 32767.0f)
                );
        }

        if (!success)
        {
            break;
        }

        int encodedBytes = lame_encode_buffer(
            encoder,
            left.data(),
            right.data(),
            static_cast<int>(currentFrames),
            mp3Buffer.data(),
            static_cast<int>(mp3Buffer.size())
        );

        if (encodedBytes < 0)
        {
            success = false;
            error = "MP3 encoding failed.";
            break;
        }

        if (encodedBytes > 0)
        {
            size_t written = fwrite(
                mp3Buffer.data(),
                1,
                encodedBytes,
                outputFile
            );

            if (written != static_cast<size_t>(encodedBytes))
            {
                success = false;
                error = "Unable to write MP3 data.";
                break;
            }
        }

        processedFrames += currentFrames;
    }

    if (success)
    {
        int encodedBytes = lame_encode_flush(
            encoder,
            mp3Buffer.data(),
            static_cast<int>(mp3Buffer.size())
        );

        if (encodedBytes < 0)
        {
            success = false;
            error = "Unable to finish MP3 encoding.";
        }
        else if (encodedBytes > 0)
        {
            size_t written = fwrite(
                mp3Buffer.data(),
                1,
                encodedBytes,
                outputFile
            );

            if (written != static_cast<size_t>(encodedBytes))
            {
                success = false;
                error = "Unable to write final MP3 data.";
            }
        }
    }

    // Запись Xing/Info-заголовка в начало файла (нужна для корректной длительности VBR)
    if (success)
    {
        vector<unsigned char> tagBuffer(8192);

        size_t tagSize = lame_get_lametag_frame(
            encoder,
            tagBuffer.data(),
            tagBuffer.size()
        );

        // Если буфера не хватило, функция возвращает нужный размер
        if (tagSize > tagBuffer.size())
        {
            tagBuffer.resize(tagSize);

            tagSize = lame_get_lametag_frame(
                encoder,
                tagBuffer.data(),
                tagBuffer.size()
            );
        }

        if (tagSize == 0)
        {
            success = false;
            error = "Unable to build MP3 duration header.";
        }
        else if (fseek(outputFile, 0, SEEK_SET) != 0 ||
            fwrite(tagBuffer.data(), 1, tagSize, outputFile) != tagSize)
        {
            success = false;
            error = "Unable to write MP3 duration header.";
        }
    }

    if (fclose(outputFile) != 0 && success)
    {
        success = false;
        error = "Unable to close output MP3 file.";
    }

    lame_close(encoder);

    return success;
}

// Универсальное сохранение аудиофайлов
bool saveAudio(
    const fs::path& filename,
    const AudioData& audio,
    string& error)
{
    wstring extension = filename.extension().wstring();

    transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        towlower
    );

    if (extension == L".wav")
    {
        return saveWavPath(filename, audio, error);
    }

    if (extension == L".mp3")
    {
        return saveMp3(filename, audio, error);
    }

    if (extension == L".flac")
    {
        return saveFlac(filename, audio, error);
    }

    error = "Unsupported output audio format.";
    return false;
}
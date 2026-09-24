#ifndef MICROPHONE_H_
#define MICROPHONE_H_

#include <cstdint>

#include <driver/i2s_std.h>

#include <tlib_tensor.h>
#include <audio_source.h>

/**
 * Wrapper around an I2S RX instance to sample audio from an I2S microphone
 */
class Microphone : public AudioSource
{
public:
    /**
     * Configures and enables the I2S microphone.
     *
     * @param   port        I2S peripheral number
     * @param   bclk_pin    Bit clock pin
     * @param   ws_pin      Word select pin
     * @param   din_pin     Data in pin
     * @param   gain        Amount of right-shift applied when converting audio samples from 32bit to 16bit
     */
    explicit Microphone(const int port, const gpio_num_t bclk_pin, const gpio_num_t ws_pin,
                        const gpio_num_t din_pin, const uint8_t gain);

    /**
     * Delete move and copy constructors/assignments
     */
    Microphone(const Microphone &) = delete;
    Microphone(const Microphone &&) = delete;
    Microphone &operator=(const Microphone &) = delete;
    Microphone &&operator=(const Microphone &&) = delete;

    /**
     * Reads I2S data from the microphone until the given buffer is exhausted.
     * The samples are read as mono int32 audio @ 16kHz into the provided buffer.
     * Afterwards, they are converted in-place to mono int16 audio @ 16kHz.
     *
     * Hence, to be able to read 16000 samples, the buffer needs a length of
     * 16000*sizeof(int32)=64000 bytes to store the raw data and after conversion to int16
     * the used buffer size will be 16000*sizeof(int16)=32000 bytes.
     *
     * @param buffer    [n] int16_t tensor, audio/data buffer.
     * @return          The number of valid samples written to the buffer.
     */
    size_t Read(tlib::Tensor<int16_t> &buffer) override;

private:
    i2s_chan_handle_t i2s_rx_channel_{}; /*!< I2S RX instance handle*/
    const uint8_t gain_;                 /*!< Right-shift applied when converting samples to 16bit */
};

#endif
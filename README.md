# GPIO Bitstream

There are some use cases when a device needs a bitstream with an arbitrary
bitrate. This is my exact use case when the bitstream with an non-standard
bitrate is transmitted on 433 MHz to the controlled device.
This device therefore allocates one GPIO pin and one character device and
bitbangs the pin with the data written to the chardev.

## Driver setup

The device is defined simply by a device tree:

```
gpio-bitstream {
	compatible = "gpio-bitstream";
	gpios = ...; /* One GPIO pin */
};
```

Yes, that is it. An udev script should then create the `/dev/gpio-bitstrem` file for you.

## Bitrate setup

After opening the device, the user must set the bitrate using `ioctl`. There
are several possible ways to do this:

* `GPIO_BITSTREAM_BPS`, `(uint32_t *) bitrate` to set a bitrate in bits per second,
* `GPIO_BITSTREAM_BPY`, `(uint64_t *) bitrate` to set a bitrate in bits per year (useful when your bitrate is not a whole number in *bps*)
* `GPIO_BITSTREAM_DELAY`, `(uint64_t *) delay` to set delay between consecutive bits in picoseconds

Note that with higher bitrates you start not only getting electrical problems
on the wire but also the kernel probably won't execute the timer fast enough.

## Transmission

After setting the bitrate, the module bitbangs its pin by data written to the
device. When no data is written, the pin is kept at zero. Until bitrate has
been set, `write` returns `-EINVAL`.

The module has an internal buffer of 4K bytes (32K bits). When the buffer is
full, the module blocks until at least half of the buffer is available to avoid
storing data one byte every time.

------

This project is currently in an early development stage, yet still available under
GNU GPL 2 on our own risk. Do your own thorough checking and testing before
using this code.

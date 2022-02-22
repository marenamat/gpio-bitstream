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

After opening the device, the user must set the bitrate by writing into a sysfs
file. There are several possible ways to do this:

* `bps` to set a bitrate in bits per second,
* `bpy`, to set a bitrate in bits per year (useful when your bitrate is not a whole number in *bps*; the *year* has exactly `60 * 60 * 24 * 365 = 31536000` seconds for this usage),
* `delay`, to set delay between consecutive bits in nanoseconds

Note that with higher bitrates you start not only getting electrical problems
on the wire but also the kernel probably won't execute the timer fast enough.
The bps and bpy values are converted to `delay`.

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

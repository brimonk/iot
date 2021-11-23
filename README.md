# BrianIOT

This is all of the code for my IOT stuff.

## Building The Micro-Controller Code

Honestly, I just used the Arduino IDE. You should be able to pull up the code in the IDE, follow
[these](https://cdn-learn.adafruit.com/downloads/pdf/adafruit-huzzah-esp8266-breakout.pdf)
instructions reasonably, and you should have a working-ish system. Keep in mind, you will need a
micro in the ESP8266 family.

## Building The Server Code

Building all of the server code (should) be as simple as running

```sh
make
```

once you've installed all of the dependencies. Eventually, when this might be something that I want
to let other people install because it has some actually decent features, I'll document just what
those dependencies are, and how you might install them on various linux distros. For now, it's
basically

* `libmagic`
* `libsodium`
* `libjansson`

And everything else should be included in the repo.

## Running

```sh
./server
```

## The Story Behind It

So, this isn't really a joke. Yes, this entire repository was setup to replace some Chinese
SmartPlug(TM) nonsense that my girl friend bought to make turning on / off Christmas lights easier.
When she sent me to setup these smart plugs, I couldn't get through the Terms + Conditions, so I
started to read. Condition **TWO** said the following:

> You shall obey all laws of the People's Republic of China while the device is in use.

So, yeah, no thanks. It's bad enough that I have to listen to the alphabet agencies that I have, let
alone foreign ones.

That's why I set this up.


# Project brief — sloopy-sloop-32

## Amaç

M-VAVE FM-1 için geliştirilmiş açık kaynak **SLOOP** firmware'ini **ESP32-S3 N16R16 + PCM5102A** platformuna native olarak portlamak.

Bu proje bir FM-1 emülatörü değildir. SLOOP'un synth engine, drum engine, sequencer, mixer, FX, preset/project ve UI state mantığı mümkün olduğunca korunacak; JieLi/FM-1 donanım bağımlılıkları ESP32 platform katmanıyla değiştirilecektir.

## İlk donanım

Sadece:

- ESP32-S3 N16R16 sınıfı kart
- PCM5102A
- USB güç/programlama
- PCM5102A stereo line out

İlk sürümde fiziksel ekran, buton, encoder, pad veya SD kart olmayacak.

## Kullanıcı arayüzü

Tarayıcı açıldığında cihazın ön paneli M-VAVE FM-1'e mümkün olduğunca yakın görünmelidir. Web panelindeki butonlar/encoderlar SLOOP'a fiziksel FM-1 kontrolleri gibi event göndermelidir.

SLOOP ekran mantığı yeniden uydurulmamalı. Upstream ekran/UI kodu analiz edilip mümkün olan en doğrudan şekilde web tarafına taşınmalı veya sanal framebuffer/display komutları browser'a aktarılmalıdır.

## Geliştirme yöntemi

Gerçek kart ilk günden zorunlu değildir.

1. Upstream SLOOP host testleri çalıştırılır.
2. Portable SLOOP core ayrıştırılır.
3. PC/native test backend'leri yazılır.
4. ESP32-S3 firmware QEMU'da boot/test edilir.
5. QEMU'nun emüle etmediği I2S/Wi-Fi için test double/backends kullanılır.
6. Port stabil olduktan sonra gerçek ESP32-S3 ve PCM5102A üzerinde I2S/Wi-Fi/realtime testleri yapılır.

## Kullanıcı tarafından sonradan değiştirilecekler

- I2S BCLK GPIO
- I2S LRCLK/WS GPIO
- I2S DATA OUT GPIO
- Wi-Fi modu
- Wi-Fi SSID/password
- serial flash port

Bunlar source code içine hard-code edilmemelidir.

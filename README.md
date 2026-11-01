# KIVRIM

[English](#english) · [Türkçe](#türkçe)

## English

KIVRIM is a C11 library and command-line toolkit for lossless file compression
and directory archiving. Archives store identical data fragments once and use
compact references to reconstruct the original files.

### Build

KIVRIM needs a C11 compiler and Make. Its compression code has no external library
dependencies. The tools run on Linux, macOS, and Windows through
[MSYS2](https://www.msys2.org/).

On Windows, open the **MSYS2 MSYS** terminal and install the build tools:

```sh
pacman -S --needed gcc make git
```

Windows builds use the MSYS2 runtime (`msys-2.0.dll`); run the tools in the same
MSYS environment. Then clone and build the project:

```sh
git clone https://github.com/crypto-su/kivrim.git
cd kivrim
make
```

| Command | Output |
|---|---|
| `make` | `build/kivrim`, `build/kivrim-archive`, `build/libkivrim.a` |
| `make archive` | `build/kivrim-archive` |
| `make clean` | Removes the build directory |

Compiler options can be supplied with `CC`, `CFLAGS`, `CPPFLAGS`, and `LDFLAGS`;
run `make clean` before changing build options. GitHub Actions builds the C
components and checks lossless roundtrips on Linux, macOS, and Windows (MSYS2).

### Compress a file

```sh
./build/kivrim c input.dat output.kvr
./build/kivrim info output.kvr
./build/kivrim d output.kvr restored.dat

# Faster compression mode: skips the sampled delta transform.
./build/kivrim c --fast input.dat fast.kvr

# Allow up to 1 GiB of decoded output.
./build/kivrim d --max-output 1073741824 fast.kvr restored-fast.dat
```

`c` uses automatic transform selection by default. `d` verifies the checksum;
`info` displays header metadata. The CLI reads the whole input into memory and
accepts input files up to 1 GiB. The default decoded-output limit is 512 MiB;
`--max-output` takes a byte count. Existing output files are never overwritten.
Use `./build/kivrim --help` or `--version` for help and version information.

The codec processes blocks of up to 64 KiB. Repeated byte sequences use backward
references; automatic mode can also apply reversible byte-delta transforms.
A block is stored raw when encoding does not reduce its payload.

### Archive a directory

```sh
./build/kivrim-archive c videos videos.kva
./build/kivrim-archive d videos.kva restored

# Optionally evaluate reversible delta transforms within new chunks.
./build/kivrim-archive c videos videos-auto.kva --auto
```

The `.kva` archive stores regular files from one directory, preserving their names
and contents. Subdirectories and symbolic links are rejected. File permissions,
timestamps, and extended attributes are not stored. The archive and extraction
directory must be new; source files must remain unchanged during compression.

Content-defined boundaries split data into fragments of 2–32 KiB, with a possible
shorter final fragment. A hash lookup followed by an exact byte comparison finds
fragments already stored in the archive. New fragments use the KIVRIM codec in
fast mode by default, or raw storage when compression is not smaller. A compact
record sequence stores the fragments and their references.
All data needed for extraction is inside the archive.

The archive format uses variable-width integers for fragment types and lengths.
References store the difference from the previous referenced fragment ID; runs of
consecutive IDs share one record. Fragment lengths and checksums are stored
with the original fragment and reused by references.

Extraction reads one fragment at a time and checks both fragment and file XXH32
checksums. Compression maps one source file at a time and keeps a fragment index
in memory. Limits are 1 GiB per file, 16 GiB of total original data, 65,535 files,
2,097,152 unique fragments, and 255 bytes per filename. Available memory and open
file limits also apply. This command creates a complete archive in one pass.

Use `./build/kivrim-archive --help` or `--version` for command information.

### Embed in C

| Header | Library | Entry points |
|---|---|---|
| [kivrim.h](include/kivrim.h) | `build/libkivrim.a` | `kvr_compress_bound`, `kvr_compress`, `kvr_decompress`, `kvr_decompressed_size`, `kvr_status_string` |

For the core API, allocate using `kvr_compress_bound`, check returned status codes,
and apply a size limit before allocating from `kvr_decompressed_size`. Compression
modes are `KVR_AUTO` and `KVR_FAST`. Include `kivrim.h` and link `libkivrim.a`
in your application.

### License

KIVRIM's original source code and documentation are licensed under the
[MIT License](LICENSE).

Copyright (c) 2026 Ugur Yildirim — [info@8bit.tr](mailto:info@8bit.tr).

[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) contains the acknowledgment and
upstream references for Yann Collet's XXH32 algorithm.

## Türkçe

KIVRIM; kayıpsız dosya sıkıştırma ve klasör arşivleme için C11 ile yazılmış bir
kütüphane ve komut satırı araçlarıdır. Arşivlerde aynı veri parçaları bir kez
saklanır; özgün dosyalar küçük referans kayıtları kullanılarak geri oluşturulur.

### Derleme

KIVRIM için C11 derleyicisi ve Make gerekir. Sıkıştırma kodunun harici kütüphane
bağımlılığı yoktur. Araçlar Linux, macOS ve [MSYS2](https://www.msys2.org/)
üzerinden Windows'ta çalışır.

Windows'ta **MSYS2 MSYS** terminalini açıp derleme araçlarını kurun:

```sh
pacman -S --needed gcc make git
```

Windows derlemeleri MSYS2 çalışma zamanını (`msys-2.0.dll`) kullanır; araçları
aynı MSYS ortamında çalıştırın. Ardından projeyi indirip derleyin:

```sh
git clone https://github.com/crypto-su/kivrim.git
cd kivrim
make
```

| Komut | Üretilen dosyalar |
|---|---|
| `make` | `build/kivrim`, `build/kivrim-archive`, `build/libkivrim.a` |
| `make archive` | `build/kivrim-archive` |
| `make clean` | Derleme dizinini temizler |

Derleyici seçenekleri `CC`, `CFLAGS`, `CPPFLAGS` ve `LDFLAGS` ile verilebilir.
Derleme seçeneklerini değiştirmeden önce `make clean` çalıştırın.
GitHub Actions, C bileşenlerini Linux, macOS ve Windows (MSYS2) üzerinde derler
ve sıkıştırılıp açılan verinin kayıpsız olduğunu kontrol eder.

### Tek dosya sıkıştırma

```sh
./build/kivrim c input.dat output.kvr
./build/kivrim info output.kvr
./build/kivrim d output.kvr restored.dat

# Örneklem üzerinden delta dönüşümü seçimini atlayan hızlı kip.
./build/kivrim c --fast input.dat fast.kvr

# Açılan dosya için 1 GiB sınırına izin ver.
./build/kivrim d --max-output 1073741824 fast.kvr restored-fast.dat
```

`c`, varsayılan olarak otomatik dönüşüm seçimini kullanır. `d`, sağlama toplamını
doğrular; `info`, başlık bilgilerini gösterir. Araç girdiyi bütünüyle belleğe alır;
girdi sınırı 1 GiB, varsayılan açma sınırı 512 MiB'dir. `--max-output` bayt cinsinden
verilir. Mevcut çıktı dosyalarının üzerine yazılmaz. Yardım için
`./build/kivrim --help`, sürüm için `./build/kivrim --version` kullanın.

Sıkıştırıcı, en fazla 64 KiB'lik blokları işler. Tekrarlayan bayt dizileri geriye
referanslarla ifade edilir; otomatik kipte geri çevrilebilir bayt farkı dönüşümleri
de uygulanabilir. Kodlama parça içeriğini küçültmüyorsa blok ham saklanır.

### Klasörü arşivleme

```sh
./build/kivrim-archive c videos videos.kva
./build/kivrim-archive d videos.kva restored

# İsteğe bağlı: yeni parçalar içinde geri çevrilebilir delta dönüşümlerini değerlendir.
./build/kivrim-archive c videos videos-auto.kva --auto
```

`.kva` arşivi, bir klasördeki normal dosyaların adlarını ve içeriklerini saklar.
Alt klasörler ve sembolik bağlantılar kabul edilmez. Dosya izinleri, zaman damgaları
ve genişletilmiş öznitelikler saklanmaz. Arşiv ve çıkarma klasörü yeni olmalıdır;
kaynak dosyalar sıkıştırma sırasında değişmemelidir.

İçeriğe göre seçilen sınırlar veriyi 2–32 KiB'lik parçalara ayırır; son parça daha
küçük olabilir. Özet tablosunda arama ve ardından tam bayt karşılaştırması, arşivde
zaten bulunan parçaları belirler. Yeni parçalar varsayılan olarak KIVRIM'in hızlı
kipiyle sıkıştırılır; küçülmeyen parçalar ham saklanır. Parçalar ve referansları
küçük kayıtlar halinde yazılır. Açmak için gereken bütün veri arşivin içindedir.

Arşiv biçimi, parça türlerini ve uzunluklarını değişken uzunluklu sayılarla yazar.
Referanslar, önceki referansın parça numarasına göre fark olarak saklanır;
ardışık numaralara giden referanslar tek kayıtta birleştirilir. Parça uzunlukları
ve sağlama toplamları, ilk saklanan parçadan alınır ve her referansta tekrar yazılmaz.

Açma işlemi parçaları sırayla okur, parça ve dosya XXH32 sağlama toplamlarını
doğrular. Sıkıştırma aynı anda bir kaynak dosyayı belleğe eşler ve parça indeksini
bellekte tutar. Sınırlar: dosya başına 1 GiB, toplam 16 GiB ham veri, 65.535 dosya,
2.097.152 farklı parça ve dosya adı başına 255 bayt. Kullanılabilir bellek ve açık
dosya sınırları da geçerlidir. Bu komut arşivi tek geçişte oluşturur.

Komut bilgileri için `./build/kivrim-archive --help` veya `--version` kullanın.

### C uygulamasına ekleme

| Başlık | Kütüphane | Başlıca fonksiyonlar |
|---|---|---|
| [kivrim.h](include/kivrim.h) | `build/libkivrim.a` | `kvr_compress_bound`, `kvr_compress`, `kvr_decompress`, `kvr_decompressed_size`, `kvr_status_string` |

Çekirdekte alanı `kvr_compress_bound` ile hesaplayın, dönüş kodlarını kontrol edin ve
`kvr_decompressed_size` sonucuyla bellek ayırmadan önce boyut sınırı uygulayın.
Sıkıştırma kipleri `KVR_AUTO` ve `KVR_FAST`'tır. Uygulamanıza `kivrim.h` başlığını
ekleyin ve `libkivrim.a` kütüphanesini bağlayın.

### Lisans

KIVRIM'in özgün kaynak kodu ve dokümantasyonu [MIT Lisansı](LICENSE) ile sunulur.

Copyright (c) 2026 Ugur Yildirim — [info@8bit.tr](mailto:info@8bit.tr).

[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) dosyasında XXH32 algoritması için
Yann Collet atfı ve resmî kaynak bağlantıları yer alır.

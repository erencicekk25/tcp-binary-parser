# tcp-binary-parser

Sahadaki sensörlerden TCP üzerinden gelen ikili (binary) paketleri okuyup çözen, küçük ve bağımlılıksız bir C++17 projesi. Protokol katmanı tek bir başlık dosyasından ibaret; projeye eklemek için `PacketProtocol.hpp` dosyasını kopyalamanız yeterli. Yanında da bu protokolü konuşan, `poll()` tabanlı hafif bir TCP sunucusu var.

Bu projeyi yazarken aklımdaki senaryo şuydu: bir mikrodenetleyici belirli aralıklarla sıcaklık ve nem ölçüyor, bunu birkaç baytlık bir pakete sarıp ağ üzerinden gönderiyor. Karşı tarafta da bu paketleri güvenle ayrıştırabilen, bozuk veri geldiğinde çökmeyen, kaynak tüketimi düşük bir alıcı gerekiyor. JSON gibi metin tabanlı formatlar bu tür cihazlar için hem bant genişliği hem de işlemci açısından fazla ağır kalabiliyor, o yüzden sabit yapılı bir ikili format tercih ettim.

## Neler var?

- **Header-only protokol kütüphanesi.** Harici bağımlılık yok, sadece standart kütüphane kullanılıyor.
- **Platformdan bağımsız serileştirme.** Çok baytlı alanlar her zaman little-endian olarak yazılıp okunuyor. Yani gönderen cihaz ile alıcı farklı bayt sıralamasına sahip olsa bile veri doğru çözülüyor.
- **Güvenli ayrıştırma.** `parseSensorPacket` geriye `std::optional<SensorPayload>` döndürüyor. Sync byte, komut kimliği, payload uzunluğu, buffer sınırları ve değer aralıkları (NaN, sonsuz, mantıksız sıcaklık/nem) tek tek kontrol ediliyor. İsterseniz neyin yanlış gittiğini `ParseError` üzerinden öğrenebiliyorsunuz.
- **TCP akışını doğru ele alan sunucu.** TCP bir akış protokolü olduğu için paketler parça parça ya da birkaç tanesi bir arada gelebilir. Sunucu her istemci için ayrı bir tampon tutuyor, eksik paketleri tamamlanana kadar bekliyor, araya karışan çöp baytları atlayıp bir sonraki sync byte'tan devam ediyor.
- **Tek thread, çoklu istemci.** `poll()` ile aynı anda birden fazla bağlantı yönetiliyor. Thread yok, kilit yok; gömülü Linux kartlarında rahatça çalışır.
- **Düzgün kapanış.** `Ctrl+C` (SIGINT) ya da SIGTERM geldiğinde sunucu açık bağlantıları kapatıp temiz bir şekilde çıkıyor. Soketler RAII ile yönetildiği için hiçbir koşulda dosya tanımlayıcısı sızmıyor.

## Paket formatı

Her paket 4 baytlık bir başlık ve ardından gelen payload'dan oluşuyor:

| Alan          | Boyut   | Açıklama                                   |
|---------------|---------|--------------------------------------------|
| `sync_byte`   | 1 bayt  | Her zaman `0xAA`. Paketin başını işaretler. |
| `cmd_id`      | 1 bayt  | Komut kimliği. Sensör verisi için `0x01`.  |
| `payload_len` | 2 bayt  | Payload uzunluğu (little-endian).          |
| payload       | değişken | `payload_len` kadar bayt.                 |

Sensör verisi (`cmd_id = 0x01`) için payload 12 bayttır:

| Alan          | Tip       | Açıklama                         |
|---------------|-----------|----------------------------------|
| `timestamp`   | `uint32`  | Ölçüm zamanı (Unix zamanı, saniye) |
| `temperature` | `float32` | Sıcaklık, °C                     |
| `humidity`    | `float32` | Bağıl nem, %                     |

Yani bir sensör paketi toplamda 16 bayt. Örneğin `timestamp = 1700000000`, `temperature = 21.5`, `humidity = 55.25` değerleri hat üzerinde şöyle görünür:

```
aa 01 0c 00  00 f1 53 65  00 00 ac 41  00 00 5d 42
└─ başlık ─┘ └timestamp┘  └sıcaklık─┘  └── nem ──┘
```

Kabul edilen değer aralıkları: sıcaklık -100 ile 200 °C, nem 0 ile 100 arası. Payload uzunluğu en fazla 1024 bayt olabilir; bu sınır, bozuk bir uzunluk alanının alıcıyı gereksiz yere büyük veri beklemeye zorlamasını engelliyor. Hepsi `PacketProtocol.hpp` içindeki `tbp::protocol` isim alanında sabit olarak tanımlı, ihtiyacınıza göre değiştirebilirsiniz.

## Dizin yapısı

```
tcp-binary-parser/
├── CMakeLists.txt
├── include/
│   ├── PacketProtocol.hpp     # Header-only protokol kütüphanesi
│   └── TcpServer.h            # Sunucu arayüzü
├── src/
│   ├── TcpServer.cpp          # Berkeley Sockets + poll() ile sunucu
│   └── main.cpp               # Çalıştırılabilir uygulama
├── tests/
│   └── PacketProtocolTests.cpp
└── tools/
    └── send_packet.py         # Deneme amaçlı küçük istemci
```

## Gereksinimler

- C++17 destekleyen bir derleyici (GCC 8+, Clang 7+ veya güncel bir MSVC)
- CMake 3.14 veya üzeri
- Sunucu için POSIX uyumlu bir sistem: Linux, macOS, WSL ya da Raspberry Pi gibi gömülü Linux kartları
- Test istemcisini kullanmak isterseniz Python 3 (zorunlu değil)

Sunucu doğrudan `sys/socket.h` kullandığı için MSVC ile derlenmez. Protokol kütüphanesi ve testleri ise MSVC'de de sorunsuz çalışır. Sunucuyu Windows makinede denemek istiyorsanız iki yol var: WSL ya da MSYS2. MSYS2 tarafında yeniden başlatma gerektirmeyen kurulum şöyle:

```bash
winget install MSYS2.MSYS2
# Ardından "MSYS2 MSYS" terminalini açıp:
pacman -S --needed gcc cmake make python
```

Bundan sonra aşağıdaki derleme adımları aynen geçerli. Dikkat: MinGW/UCRT64 değil, **MSYS** ortamını kullanmanız gerekiyor; POSIX soket desteği yalnızca orada var.

## Derleme

```bash
git clone https://github.com/erencicekk25/tcp-binary-parser.git
cd tcp-binary-parser

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Derleme bittiğinde çalıştırılabilir dosya `build/tcp_binary_parser` konumunda olur.

Kullanabileceğiniz CMake seçenekleri:

| Seçenek                  | Varsayılan | Ne işe yarar                                  |
|--------------------------|------------|-----------------------------------------------|
| `TBP_BUILD_SERVER`       | `ON`       | TCP sunucusunu derler (yalnızca POSIX)        |
| `TBP_BUILD_TESTS`        | `ON`       | Protokol testlerini derler                    |
| `TBP_WARNINGS_AS_ERRORS` | `OFF`      | Derleyici uyarılarını hataya çevirir          |

Örneğin CI ortamında uyarıları hata olarak görmek isterseniz:

```bash
cmake -S . -B build -DTBP_WARNINGS_AS_ERRORS=ON
```

### Çapraz derleme

Gömülü bir kart için derlerken kendi toolchain dosyanızı vermeniz yeterli, proje başka bir şey istemiyor:

```bash
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=/yol/arm-linux-gnueabihf.cmake
cmake --build build-arm
```

## Çalıştırma

Sunucuyu varsayılan ayarlarla (tüm arayüzler, 9000 portu) başlatmak için:

```bash
./build/tcp_binary_parser
```

Komut satırı seçenekleri:

```
-p, --port <port>      Dinlenecek port (varsayılan: 9000, 0 verilirse sistem boş bir port seçer)
-b, --bind <adres>     Bağlanılacak IPv4 adresi (varsayılan: 0.0.0.0)
-c, --max-clients <n>  Aynı anda kabul edilecek en fazla istemci sayısı (varsayılan: 32)
    --self-test        Örnek bir paketi kodlayıp tekrar çözer, sonra çıkar
-h, --help             Yardım metnini gösterir
```

Örneğin sadece yerel bağlantıları 7000 portundan kabul etmek için:

```bash
./build/tcp_binary_parser --bind 127.0.0.1 --port 7000
```

Sunucuyu durdurmak için `Ctrl+C` yeterli.

### Hızlı kontrol

Ağa hiç dokunmadan kütüphanenin doğru çalışıp çalışmadığını görmek için:

```bash
./build/tcp_binary_parser --self-test
```

Bu komut örnek bir paket oluşturur, baytlarını ekrana basar, sonra paketi geri çözüp değerlerin birebir aynı olduğunu kontrol eder.

### Gerçek veriyle deneme

Bir terminalde sunucuyu açın, diğerinde depodaki test istemcisini çalıştırın:

```bash
python3 tools/send_packet.py --port 9000 --count 5
```

Sunucu tarafında şuna benzer bir çıktı görmelisiniz:

```
Listening on 0.0.0.0:9000
[client 1 127.0.0.1:51724] connected
[client 1 127.0.0.1:51724] timestamp=1762430400 temperature=24.37 C humidity=52.10 %
[client 1 127.0.0.1:51724] timestamp=1762430400 temperature=19.82 C humidity=61.45 %
...
[client 1 127.0.0.1:51724] disconnected
```

İstemcide sunucunun zor durumları nasıl karşıladığını görmek için iki seçenek daha var:

- `--fragment`: Paketleri bayt bayt gönderir. Sunucu parçaları birleştirip paketi yine eksiksiz çözer.
- `--noise`: Her paketin önüne rastgele baytlar ekler. Sunucu bunları atlayıp bir sonraki geçerli pakete senkronize olur ve kaç bayt attığını uyarı olarak yazar.

Python kullanmak istemezseniz `nc` ile ham bayt da gönderebilirsiniz:

```bash
printf '\xaa\x01\x0c\x00\x00\xf1\x53\x65\x00\x00\xac\x41\x00\x00\x5d\x42' | nc 127.0.0.1 9000
```

Bazı `nc` sürümlerinde gönderimden sonra bağlantının kapanması için `-q 1` ya da `-N` parametresini eklemeniz gerekebilir.

## Testler

```bash
ctest --test-dir build --output-on-failure
```

Testler; gidiş-dönüş serileştirmeyi, hat üzerindeki bayt sırasını ve her bir hata durumunu (eksik buffer, fazla bayt, yanlış sync byte, bilinmeyen komut, uzunluk uyuşmazlığı, aşırı büyük payload, geçersiz sensör değerleri) ayrı ayrı kontrol ediyor. Harici bir test framework'ü kullanmadım, böylece proje hiçbir şey indirmeden derleniyor.

## Kütüphaneyi kendi projenizde kullanmak

En basit yol `include/PacketProtocol.hpp` dosyasını projenize kopyalamak. CMake kullanıyorsanız depoyu alt dizin olarak da ekleyebilirsiniz:

```cmake
set(TBP_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(TBP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/tcp-binary-parser)

target_link_libraries(uygulamam PRIVATE tbp::packet_protocol)
```

Gönderen tarafta paket oluşturmak:

```cpp
#include "PacketProtocol.hpp"

const tbp::SensorPayload reading{timestamp, 22.4F, 47.0F};
const auto packet = tbp::PacketParser::createSensorPacket(reading);
// packet.data() ve packet.size() ile doğrudan soketten gönderebilirsiniz.
```

Alıcı tarafta çözmek:

```cpp
tbp::ParseError error = tbp::ParseError::None;
const auto reading = tbp::PacketParser::parseSensorPacket(buffer, size, &error);

if (!reading) {
    std::cerr << "Paket reddedildi: " << tbp::toString(error) << '\n';
    return;
}

std::cout << reading->temperature << " C\n";
```

`ParseError` parametresi isteğe bağlı. Hatanın sebebiyle ilgilenmiyorsanız sadece `std::optional` dönüşünü kontrol etmeniz yeterli.

Kütüphanedeki fonksiyonlar `noexcept`, istisna fırlatmıyor ve dinamik bellek ayırmıyor. `createSensorPacket` sonucu sabit boyutlu bir `std::array` olarak döndürüyor. Bu sayede istisnaların ya da heap'in kapalı olduğu mikrodenetleyici ortamlarında da gönül rahatlığıyla kullanabilirsiniz.

## Yeni bir paket tipi eklemek

1. `CommandId` enum'una yeni bir değer ekleyin.
2. Payload için `#pragma pack(push, 1)` bloğu içinde yeni bir struct tanımlayın ve boyutunu `static_assert` ile sabitleyin.
3. `PacketParser` içine, mevcut sensör fonksiyonlarına benzer şekilde `create...` ve `parse...` metodlarını yazın.
4. `TcpServer::dispatchFrame` içindeki `switch` bloğuna yeni komutu ekleyin.

Sunucunun akış ve senkronizasyon mantığı payload tipinden bağımsız, yani bu kısma dokunmanız gerekmiyor.

## Hata yönetimi hakkında

Projede iki farklı yaklaşım var ve bu bilinçli bir tercih:

- **Protokol katmanı** istisna kullanmıyor. Her hata `std::optional` ve `ParseError` ile dönüyor. Ağdan gelen bozuk veri olağan bir durum, istisnai bir durum değil.
- **Sunucu kurulumu** (`socket`, `bind`, `listen`) başarısız olursa `std::system_error` fırlatılıyor, çünkü bu noktada devam etmenin bir anlamı yok. `main` bu hataları yakalayıp okunaklı bir mesajla çıkıyor.

Çalışma sırasında tek bir istemcide yaşanan sorun (okuma hatası, tampon taşması, bozuk paket, kullanıcı callback'inin fırlattığı istisna) yalnızca o istemciyi etkiliyor. Diğer bağlantılar ve sunucunun kendisi çalışmaya devam ediyor.

## Bilinen sınırlamalar

- Şu an yalnızca IPv4 destekleniyor.
- Pakette checksum ya da CRC alanı yok. TCP kendi bütünlük kontrolünü yaptığı için çoğu senaryoda bu yeterli. Ancak veri yol üzerinde bir seri hat ya da başka bir ara katmandan geçiyorsa başlığa bir CRC-16 alanı eklemenizi öneririm.
- Sunucu tek thread'de çalışıyor. Yüzlerce eşzamanlı cihaz için `epoll` ya da benzeri bir mekanizmaya geçmek daha mantıklı olur.
- Bağlantılarda şifreleme yok. Güvenilmeyen bir ağda kullanacaksanız trafiği VPN ya da TLS sonlandırıcı bir proxy arkasına almanız gerekir.

## Lisans

Bu proje MIT lisansı ile dağıtılıyor. Ticari projeler dahil dilediğiniz gibi kullanabilir, değiştirebilir ve dağıtabilirsiniz; tek şart lisans metnini korumanız. Ayrıntılar için [LICENSE](LICENSE) dosyasına bakabilirsiniz.

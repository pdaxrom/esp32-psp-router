# PSP Router 2.0 — ESP32-S3 USB WAN / Wi-Fi LAN

Прошивка на C / ESP-IDF для Super Mini ESP32-S3 HW-747, проверенного чипа
ESP32-S3FH4R2: **4 МиБ Flash, 2 МиБ Quad SPI PSRAM**. Дополнительные аппаратные
модули не нужны. Компьютер раздаёт интернет через native USB-C
(**GPIO19 D−, GPIO20 D+**); ESP маршрутизирует трафик PSP.

```text
Интернет → компьютер с Internet Sharing / ICS
                 │ USB RNDIS (Windows) / CDC-ECM (macOS/Linux)
                 │ WAN ESP: DHCP-клиент, IP/шлюз/DNS от компьютера
            ESP32-S3: IPv4 NAPT
                 │ Wi-Fi LAN: 192.168.50.1/24, свой DHCP и DNS relay
                 │ PSP-NET / 802.11b / канал 6 / WPA1-PSK TKIP
              PSP-3000
```

Вариант 1, прозрачный bridge, сохранён отдельным режимом сборки. Проверенный
релиз 1.0.0 находится в коммите `692b7ec`, его аппаратный отчёт —
[docs/TEST_REPORT_V1.md](docs/TEST_REPORT_V1.md).

## Сеть и браузерный прокси

По умолчанию Wi-Fi имеет **192.168.50.1/24**. PSP получает адрес, шлюз и DNS от
ESP. USB получает отдельную аренду компьютера; например, macOS выдаёт WAN ESP
`192.168.2.x/24`, а шлюз/DNS компьютера — `192.168.2.1`. Это две разные подсети.
При пересечении WAN с LAN выбирается другая частная /24: 192.168.51.0,
172.31.50.0 или 10.77.0.0. После смены LAN PSP переподключается для новой аренды.
Если подходящей подсети нет, NAPT не включается.

Без настройки прокси PSP использует обычный NAT: приложения, игры и HTTP идут
напрямую. Для старого браузера есть отдельный **HTTP→HTTPS шлюз на ESP**, порт
**8080**. Он обращается к сайтам по HTTPS, проверяет цепочку, имя и сроки
сертификата с корневыми CA из ESP-IDF. Часы ESP синхронизируются через SNTP;
до получения времени шлюз возвращает 503 с предложением повторить запрос.
Проверка сертификатов не отключается.

Включена штатная поддержка перекрёстно подписанных цепочек CA
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`). Она нужна, в частности,
для цепочки Google WE1 → GTS Root R4, которую могут передавать сайты за Cloudflare.
Без неё IDF 6.x может отвергать корректную цепочку, даже если её корень есть в
bundle. [Описание Espressif](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_crt_bundle.html#cross-signed-certificate-support).

PSP получает страницы по HTTP. Радиоканал PSP↔ESP шифруется WPA1/TKIP;
HTTPS между ESP и сайтом завершается на ESP, сквозного TLS до PSP здесь нет.
Просроченные корни PSP при таком обращении не используются.

Шлюз предназначен для **простых публичных страниц**:

- GET и HEAD; upstream всегда HTTPS, включая запросы `http://` от PSP.
- HTTPS-редиректы и буквальные `https://` в HTML/CSS/JavaScript заменяются на HTTP,
  чтобы следующие обращения браузера тоже проходили через шлюз.
- Ответы передаются потоком как HTTP/1.0 с закрытием соединения; chunked upstream
  декодирует HTTP-клиент ESP. Запрашивается `Accept-Encoding: identity`.
- Два одновременных запроса; заголовки запроса до 4 КиБ, URL до 2 КиБ, ответ до 4 МиБ,
  цикл передачи до 60 с, отдельные сетевые операции ограничены таймаутами.
  Крупные TLS-буферы могут размещаться в PSRAM.
- POST, CONNECT, cookies/авторизация, преобразование изображений, современный JS,
  CAPTCHA и видеосервисы не поддерживаются. Бинарные ответы не переписываются.
- Ошибка сертификата/HTTPS даёт 502; автоматического обхода проверки или отката
  на незашифрованный upstream нет. HTTP-only сайты открывайте без прокси.

На PSP подтверждено открытие ya.ru, zond.am и pdaxrom.org. Современная вёрстка
последних двух отображается некорректно: шлюз передаёт HTML/CSS, преобразование
оформления для старого браузера не выполняется.

CONNECT лишь передаёт TLS между PSP и сайтом и не исправляет старые сертификаты
или несовместимые версии TLS, поэтому он не используется. В браузере вводите
**http://**, например `http://example.com/`, даже если сайт работает по HTTPS.

На `http://192.168.50.1:8080/` находится простая стартовая страница, поле адреса
и последние замеры. Connection включает DNS/TCP/TLS, Total также включает
передачу клиенту; это не отдельный бенчмарк криптографии.

На проверенной ESP32-S3 три HTTPS-загрузки по 64 КиБ через USB заняли
4,26–4,32 с каждая, включая 2,90–2,93 с установки соединения: около 15 КиБ/с
за весь запрос. Это измерение с USB-компьютера; скорость браузера PSP и Wi-Fi
пока не измерена. Условия и выполненные проверки — в [TEST_REPORT.md](TEST_REPORT.md).

DNS relay и прокси принимают запросы только от Wi-Fi-подсети и USB-компьютера
(его адрес определяется как DHCP gateway). Остальные источники отклоняются.
На компьютере их можно проверить через WAN-адрес ESP без подключения к PSP-NET.

## Плата и восстановление

Flash: **4 МиБ QIO / 80 МГц**. PSRAM: **2 МиБ Quad SPI / 40 МГц**, не OPI.
CPU в режиме router: **240 МГц**, аппаратные AES/SHA/RSA включены в IDF.
ROM проверенной платы определил Flash XMC 4MB и PSRAM AP_3v3 2MB; приложение
измерило 4194304/2097152 байт. Перед запуском AP эти физические размеры проверяются;
при несовпадении AP не запускается. Название HW-747 само по себе комплектацию
не гарантирует. Для другого чипа нужна отдельная конфигурация.

Приложение не управляет GPIO0/BOOT, EN/RESET или eFuse. Secure Boot и Flash
Encryption не включены. Native ROM download остаётся доступен через BOOT/RESET.
В обычной прошивке USB используется только как сеть: COM/tty-консоли нет.

## Зафиксированные зависимости и примеры

| Зависимость | Проверенная версия |
| --- | --- |
| ESP-IDF | v6.1-dev-4811-gfce70533fa |
| IDF commit | fce70533fa1fdeef9f903ce5e87ff8d292be992b |
| espressif/esp_tinyusb | 2.0.1~1 |
| espressif/tinyusb | 0.21.0~4 |
| Xtensa GCC | 15.2.0, esp-15.2.0_20251204 |
| Python (сборка 2.0.1) | 3.15.2 |
| Component Manager | 3.1.2 |
| esp-idf-kconfig | 3.14.0 |
| esptool | 5.6.0 |

Manifest и registry-хэши — в `main/idf_component.yml` и `dependencies.lock`.
HTTP-клиент, TLS, CA bundle, SNTP и NAPT входят в закреплённую ESP-IDF; новых
внешних библиотек нет. CMake проверяет commit IDF. `PSP_ALLOW_UNTESTED_IDF=ON`
разрешает другую ревизию только для исследования с повторной проверкой.

Использованы официальные исходники именно этой ревизии:

- [softap_sta / NAPT, DNS DHCP](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/wifi/softap_sta)
- [esp_http_client](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/protocols/esp_http_client)
- [network/bridge](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/network/bridge)
- [tusb_ncm / установка native TinyUSB](https://github.com/espressif/esp-idf/tree/fce70533fa1fdeef9f903ce5e87ff8d292be992b/examples/peripherals/usb/device/tusb_ncm)
- [TinyUSB net_lwip_webserver / ECM и RNDIS](https://github.com/hathach/tinyusb/tree/0.21.0/examples/device/net_lwip_webserver)
- [Microsoft OS 2.0 descriptors](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/microsoft-os-2-0-descriptors-specification)

Managed-компоненты и установленная ESP-IDF не патчатся. EAPOL v1 для WPA1/TKIP
задаётся linker-wrapper, собранным с тем же private ABI, что и wpa_supplicant.

## Сборка

```sh
. "$HOME/Work/ESP/esp-idf/export.sh"
sh tools/build.sh ecm                 # macOS/Linux, WPA1, router + proxy
sh tools/build.sh rndis               # Windows, WPA1, router + proxy
sh tools/build.sh ecm wpa1 bridge      # прежний прозрачный bridge
sh tools/build.sh ecm open router     # явно открытая диагностическая сеть
```

Аргументы: `[rndis|ecm] [wpa1|open] [router|bridge]`.
Каталоги отдельные: `build-ecm-router`, `build-rndis-router`,
`build-ecm-bridge`, `build-ecm-open-router`. Профили ОС выбираются при сборке;
автоматического переключения защиты нет. VID/PID прототипа: RNDIS 303a:4011,
ECM 303a:4012. Перед распространением устройства нужна назначенная VID/PID-пара.

Пароль по умолчанию **psp3000wifi**. Изменение:

```sh
idf.py -B build-ecm-router -DSDKCONFIG=build-ecm-router/sdkconfig menuconfig
# PSP USB router → WPA1 passphrase
sh tools/build.sh ecm
```

Допустимы 8–63 печатных ASCII-символа. Неверный пароль останавливает AP.
Другие параметры сети/прокси — в том же menuconfig.
Профили ОС и архитектуры меняйте аргументами скрипта, чтобы сохранять отдельные
конфигурации. В Windows PowerShell с активированной средой ESP-IDF:

```powershell
idf.py -B build-rndis-router -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-rndis-router/sdkconfig build
python tools/check_build.py build-rndis-router --profile rndis --mode router
```

## Прошивка

1. Подключите native USB-C кабелем с передачей данных.
2. Зажмите BOOT, нажмите и отпустите RESET, затем отпустите BOOT.
3. Прошейте нужную сборку (подставьте ROM-порт, COMx в Windows):

   ```sh
   idf.py -B build-ecm-router -DSDKCONFIG=build-ecm-router/sdkconfig \
     -p /dev/cu.usbmodemXXXX flash
   ```

4. Если приложение не запустилось автоматически, нажмите RESET без BOOT.
   ROM-порт исчезнет и появится USB-сеть; отсутствие последовательного порта ожидаемо.

Полный образ `dist/psp-router-ecm-wpa1.bin` записывается по **0x0**.
Один `build-ecm-router/psp_usb_router.bin` — только приложение по **0x10000**.
Полный объединённый образ перезаписывает и промежутки, включая NVS.
Для обновления без стирания NVS используйте `idf.py flash`.
Сохранённый рабочий bridge 1.0.0: `dist/psp-bridge-ecm-v1.0.0.bin`.

## Общий доступ компьютера

Компьютер должен иметь отдельное действующее подключение к интернету. USB-карта
ESP — получатель общего доступа. В роутерном режиме DHCP компьютера выдаёт адрес
только WAN ESP; адреса PSP выдаёт уже сам ESP.

### Windows 10/11 — RNDIS

В диспетчере устройств должен появиться сетевой адаптер RNDIS/Remote NDIS.
Используется штатный драйвер с Microsoft OS 2.0 Compatible ID. При необходимости:
«Обновить драйвер → Выбрать из списка → Сетевые адаптеры → Microsoft → Remote NDIS
Compatible Device». Сторонний драйвер не требуется, но привязка требует проверки
на конкретной Windows.

`Win+R` → `ncpa.cpl` → свойства действующего интернет-адаптера → «Доступ» →
разрешить общий доступ и выбрать USB RNDIS ESP. Обычно USB-сеть ICS —
192.168.137.0/24. Не назначайте USB-адаптеру внешний default gateway.

### macOS — CDC-ECM

«Настройки системы → Сеть»: PSP Router ECM/USB Ethernet, штатный драйвер.
«Основные → Общий доступ → Общий интернет»: источник — действующий интернет,
получатель — **USB Ethernet ESP**. Wi-Fi получателем не выбирайте, AP создаёт ESP.

В этой системе используется `bridge100` с адресом 192.168.2.1, участник en13.
Link-local 169.254.x.x на en13 сам по себе не является ошибкой.
В macOS 27 обычный ifconfig может скрывать MAC как 02:00:00:00:00:00;
`sudo ifconfig en13` показывает реальный адрес. [Подтверждение Apple](https://developer.apple.com/forums/thread/841958).

### Ubuntu / NetworkManager — ECM

Выберите обнаруженный USB Ethernet, создайте отдельное подключение с
`ipv4.method shared`, сохранив внешний интернет-интерфейс:

```sh
nmcli device status
sudo nmcli connection add type ethernet ifname enxXXXXXXXXXXXX con-name psp-usb \
  ipv4.method shared ipv6.method disabled
sudo nmcli connection up psp-usb
```

Подставьте свой USB-интерфейс. NetworkManager выдаст WAN ESP адрес своей подсети.

## Подключение PSP

**ARK-5: выключите WPA2 Support в настройках CFW.** На проверенной PSP именно
включённый WPA2 вызывал циклический обмен ключами и таймаут при WPA1/TKIP.
После отключения прежний релиз успешно работал.

1. Включите WLAN, Infrastructure Mode → Scan → **PSP-NET**.
2. Выберите **WPA-PSK (TKIP)**, пароль **psp3000wifi**.
3. IP и DNS — Automatic, для прямого NAT-доступа Proxy — Do Not Use.
4. В режиме router ожидается IP 192.168.50.x, шлюз/DNS 192.168.50.1.
5. Для браузерного шлюза отредактируйте это подключение: Proxy → Use,
   Address **192.168.50.1**, Port **8080**. При смене LAN используйте новый шлюз PSP.
6. Откройте **http://192.168.50.1:8080/**, затем **http://httpbingo.org/html**.
   Не вводите https:// в адресной строке: такой запрос потребует старого TLS PSP.

Можно сохранить два соединения с одним SSID: одно без прокси, второе с прокси.

## Проверки и отключения

```sh
sh tools/test_host.sh
python tools/check_build.py build-ecm-router --profile ecm --mode router
python tools/check_build.py build-ecm-bridge --profile ecm --mode bridge
python tools/test_router_live.py WAN_IP_ESP --report dist/router-live.json
```

Проверяются границы пароля/USB-кадров, пересечения CIDR, HTTP/URL-парсер,
отклонение неоднозначных запросов, переписывание ссылок на всех границах буфера,
готовность ECM после reset/alt 0/alt 1. Проверка ELF читает реальные USB-дескрипторы,
выбранный сетевой режим, CA bundle и отсутствие отладочных функций.

USB reset/suspend меняет номер сеанса, отбрасывает старые кадры и останавливает
WAN DHCP. AP/LAN остаётся доступен. При изменении WAN NAPT-таблица очищается,
активные запросы шлюза завершаются с ограниченным таймаутом; новые идут через
актуальный адрес/DNS. Старые TCP-сеансы могут закрыться. При физическом извлечении
единственного USB-кабеля плата теряет питание.

DNS relay поддерживает UDP-пакеты до 1232 байт; DNS-over-TCP не реализован.
Выполненные аппаратные проверки, замеры и оставшиеся тесты — в [TEST_REPORT.md](TEST_REPORT.md).

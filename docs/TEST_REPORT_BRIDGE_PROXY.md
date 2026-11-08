# Bridge + proxy 2.2.0

Новый режим `bridge-proxy` собирается отдельно от `bridge` и `router`.
Проверки старого bridge/WPA2 2.1.3 не подтверждают автоматически новый образ.

## Схема

- USB RNDIS/ECM и Wi-Fi объединены lwIP bridgeif в одну IPv4 подсеть.
- DHCP-клиент ESP имеет hostname `psp-proxy`. DHCP-сервер, шлюз и основной
  DNS PSP остаются на компьютере. NAPT/IP forwarding в ESP выключены.
- Перед отправкой Wi-Fi кадра в USB проверяется точный UDP/53 DNS-запрос
  `psp-proxy` от ассоциированной Wi-Fi станции. Остальные кадры не меняются.
  Ответ использует текущий DHCP IP ESP, исходные transaction ID и адрес DNS,
  TTL 0. AAAA получает пустой успешный ответ; `.local` не перехватывается.
- Кадр ответа копируется в очередь из 8 элементов и отправляется отдельной
  задачей через AP. TCP/IP callback не ждёт USB и не сохраняет входной буфер.
  Очередь имеет эпоху сети; ответ после смены адреса/USB не отправляется.
- UDP/53 на IP самой ESP отвечает только за это имя и только локальным
  источникам. Остальные имена не обслуживаются этим endpoint.
- Для TCP-ответов самой ESP поддерживается до 4 ARP-привязок подключённых
  станций; gateway ESP не может быть такой записью. При уходе станции или
  смене USB/подсети удаляются только записи, созданные этим кодом.
- HTTP→HTTPS proxy тот же: 8080, GET/HEAD, 2 запроса, CA bundle и проверки
  имени/срока сертификата, SNTP, переписывание HTTPS-ссылок в HTTP.

## Выполнено на компьютере

- Host-тесты с AddressSanitizer/UndefinedBehaviorSanitizer: DNS wire format,
  TTL/смена адреса, регистр имени, AAAA/ANY, EDNS, контрольные суммы IPv4/UDP,
  IP options/padding, усечённые/фрагментированные/чужие кадры, границы буфера,
  20 000 повреждённых пакетов; прежние тесты parser/rewrite/subnet/ECM.
- ECM/WPA2 bridge-proxy: сборка `esp32s3` и проверка ELF/дескрипторов.
  4 МиБ Flash / 2 МиБ Quad PSRAM, CPU 240 МГц, WPA2/AES, PMF выключен.
- Зависимости остаются закреплены: ESP-IDF
  `fce70533fa1fdeef9f903ce5e87ff8d292be992b`, esp_tinyusb `2.0.1~1`,
  tinyusb `0.21.0~4`. Новых библиотек нет; исходники IDF не изменены.

Дополнительно собраны и прошли ELF-проверки:

| USB | Защита | Режим | Результат |
| --- | --- | --- | --- |
| ECM | WPA2 | bridge-proxy | PASS |
| RNDIS | WPA2 | bridge-proxy | PASS |
| ECM | WPA1 | bridge-proxy | PASS |
| ECM | OPEN (явный выбор) | bridge-proxy | PASS |
| ECM | WPA2 | bridge | PASS |
| ECM | WPA1 | bridge | PASS |
| ECM | WPA2 | router | PASS |
| ECM | WPA1 | router | PASS |

WPA2 использует штатный EAPOL v2; WPA1 — прежний ограниченный init-wrapper.
ELF-проверка подтверждает отсутствие NAPT/forwarding и диагностических
перехватчиков в bridge-proxy, сохранение TLS verify и ROM-восстановления.

В `dist` подготовлены отдельные ECM/RNDIS WPA2 образы
`psp-bridge-proxy-<usb>-wpa2-v2.2.0.bin`, manifest
`bridge-proxy-v2.2.0-manifest.json` и `BRIDGE_PROXY_SHA256SUMS`.
ECM app — 811 264 байта, SHA-256
`a87457bf5bac26b8026f619ebd2a9937249151a4d3b9e06ed53f492d1172c5fc`.
Сохранённые четыре bridge-образа 2.1.3 повторно сверены с прежними SHA-256.

## Выполнено на ESP32-S3

- ECM/WPA2 bridge-proxy 2.2.0 прошита в ESP32-S3 с ROM serial
  `D0:CF:13:30:15:4C`: bootloader/partition/app, все хэши проверены,
  esptool exit 0, NVS сохранена. Запуск через watchdog reset.
- macOS перечислила ECM `303a:4012`, serial `D0CF1330154D`; en13 — active,
  член bridge100. Компьютер остаётся `192.168.2.1/24`.
- В `/var/db/dhcpd_leases` ESP получила `192.168.2.4` с hostname **psp-proxy**.
- С USB-компьютера пройдены живые проверки:
  - локальная HTTP-страница и SNTP;
  - прямой UDP DNS к ESP: `psp-proxy A` → `192.168.2.4`, TTL 0;
    отдельный endpoint не отвечает за постороннее `example.com`;
  - стартовая страница по absolute URI `http://psp-proxy:8080/?test=...`;
  - HTTPS, переписывание HTML-ссылок и Location без выполнения редиректа;
  - валидный sha256.badssl.com принят; expired/wrong.host/self-signed отклонены;
  - zond.am: HTTP 200 / 47 863 байта; pdaxrom.org: HTTP 301 / 309 байт;
  - GET/HEAD параллельно, CONNECT 405, отсутствие debug-маршрутов;
  - три HTTPS-ответа по 65 536 байт: 4,18–4,29 с, 14,91–15,30 КиБ/с;
    установка соединения DNS/TCP/TLS 2,79–2,82 с.
- Результаты: `dist/flash-bridge-proxy-ecm-wpa2-v2.2.0.log`,
  `dist/live-bridge-proxy-v2.2.0.log` и `.json`.

Прямой DNS-запрос компьютера к ESP проверяет отдельный локальный endpoint,
а HTTP absolute URI — обработку имени в proxy. Перехват Wi-Fi DNS подтверждён
отдельной проверкой PSP ниже.

## Выполнено на PSP-3000

- Пользователь подтвердил открытие **обеих страниц**:
  `http://psp-proxy:8080/?check=1` и `http://zond.am/` с proxy
  **psp-proxy:8080**, AES-профилем ARK-5 и автоматическими IP/DNS.
- ESP после проверки показывает **9 перехваченных DNS-ответов**, отдельно
  от 1 прямого DNS-запроса USB-компьютера. Это подтверждает разрешение имени
  через Wi-Fi bridge, а не только прямой endpoint ESP.
- PSP MAC `00:23:4e:17:88:02`, client IP и ARP **192.168.2.5**; ESP
  остаётся **192.168.2.4** в общей подсети компьютера.
- Снимок состояния: `dist/bridge-proxy-psp-confirmed.html`, результат —
  `dist/bridge-proxy-psp-confirmed.json`.
- После отключения и подключения USB пользователь подтвердил открытие
  **свежих адресов** `http://psp-proxy:8080/?check=2` и
  `http://zond.am/?psp_check=2` с прежним AES-профилем и proxy по имени.
  Новые параметры URL исключают повторное отображение прежних страниц из кеша.
- После нового запуска ESP: HTTP 200 с USB-компьютера, **2 перехваченных
  DNS-ответа**, 0 прямых DNS-запросов; PSP снова видна с IP/ARP
  **192.168.2.5**. Подтверждены повторное разрешение имени и восстановление proxy
  после USB-переподключения. Снимки — `dist/bridge-proxy-after-usb.html` и `.json`.

## Осталось проверить

- PSP: прямой HTTP с отключённым proxy, прежние адрес/шлюз/DNS компьютера.
- Изменение DHCP-адреса ESP: новый DNS-ответ без старого IP/старой ARP-записи.
- Windows/RNDIS и Linux: перечисление устройства и штатный общий доступ.

TTL 0 снижает кеширование устаревшего адреса; поведение собственного DNS-кеша
PSP при смене DHCP-аренды требует аппаратной проверки. Прохождение host-тестов
не является проверкой Wi-Fi, радиопередачи или браузера PSP.

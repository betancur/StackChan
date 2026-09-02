# Claude Desktop Buddy en Stack-Chan

Esta app convierte al Stack-Chan en un "Hardware Buddy" para Claude Desktop, siguiendo el protocolo de [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy). El escritorio publica el estado de las sesiones de Claude Code por Bluetooth Low Energy y el robot lo representa con la cara, los servos y los LEDs. Desde el robot se pueden aprobar o denegar las solicitudes de permiso de herramientas.

## Requisitos

- Claude para macOS o Windows con Developer Mode activado (Help → Troubleshooting → Enable Developer Mode).
- La ventana Hardware Buddy (Developer → Open Hardware Buddy…).
- Un Stack-Chan con este firmware. El robot se anuncia como `Claude StackChan-XXXX`, donde `XXXX` son los últimos cuatro dígitos de su dirección MAC.

## Primer uso

1. Abrir la app **Buddy** en el launcher del robot. La cara aparece dormida con el texto "Zzz" mientras espera al escritorio.
2. En Hardware Buddy pulsar **Connect** y elegir el robot en la lista.
3. El robot muestra un panel con un código de seis dígitos. Escribirlo en el diálogo de emparejamiento de macOS.
4. A partir de ahí la conexión es cifrada y autenticada. Las reconexiones posteriores no piden código porque la clave queda guardada en la memoria no volátil del robot.

Si macOS pierde la clave (por ejemplo tras restaurar el sistema), basta con pulsar **Forget** en Hardware Buddy y volver a conectar. El robot detecta el fallo de cifrado, borra su copia del vínculo y vuelve a mostrar un código nuevo.

## Estados

| Estado | Cuándo | Qué hace el robot |
|---|---|---|
| sleep | Sin conexión con el escritorio | Cara dormida, cabeza abajo, LEDs apagados. Tras cinco minutos baja el brillo de la pantalla. |
| idle | Conectado y sin actividad | Movimientos y expresiones aleatorias de reposo. |
| busy | Hay sesiones generando | Gota de sudor, movimiento lento, LEDs azules respirando. La burbuja muestra el resumen de una línea que envía el escritorio. |
| attention | Una herramienta espera permiso | Cara de duda, mira hacia arriba, LEDs naranja parpadeando, botones Approve y Deny en pantalla. |
| celebrate | Sube de nivel (cada 50K tokens) | Cara feliz, meneo de cabeza, LEDs arcoíris, texto "Level N!". |
| dizzy | Se sacude el robot | Ojos en espiral durante unos segundos. |
| heart | Aprobación en menos de cinco segundos | Corazones flotando y LEDs rosa. |

## Controles

- Tocar la pantalla abre un panel con el número de sesiones, tokens acumulados y del día, nivel, estadísticas de aprobaciones y las últimas líneas del transcript. Se cierra solo a los diez segundos o al tocarlo.
- Con una solicitud pendiente, tocar la cabeza del robot equivale a aprobar.
- El indicador de inicio cierra la app y devuelve al launcher (warm reboot: NimBLE no puede reiniciarse en caliente).

## Protocolo

La comunicación usa el servicio Nordic UART (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`). Cada mensaje es un objeto JSON en una línea terminada en `\n`. El escritorio envía fotos de estado cada vez que algo cambia y como mínimo cada diez segundos. Si pasan treinta segundos sin recibir ninguna, el robot considera perdido el enlace y vuelve a dormir.

Mensajes que el robot entiende:

- Foto de estado: sesiones, resumen, entradas de transcript, tokens y solicitud pendiente (`prompt`).
- Evento de turno (`evt: turn`) con el texto de la última respuesta del asistente.
- Sincronización de hora (`time`) con zona horaria.
- Comandos `status`, `name`, `owner` y `unpair`.
- Los comandos de carga de paquetes de personajes (`char_begin`, `file`, `chunk`, `file_end`, `char_end`) se rechazan con un acuse negativo, porque el Stack-Chan dibuja su propio avatar en lugar de reproducir GIFs.

Mensajes que el robot envía:

- Decisión de permiso: `{"cmd":"permission","id":"…","decision":"once"|"deny"}`.
- Acuse de cada comando: `{"ack":"<cmd>","ok":true|false,"n":0}`.
- Respuesta de `status` con nombre, batería, memoria libre, tiempo encendido, estadísticas y `sec:true` cuando el enlace está cifrado.

## Persistencia

En NVS (namespace `buddy`) se guardan el nombre del dispositivo, el nombre del propietario y los contadores de aprobaciones, denegaciones, aprobaciones rápidas y siestas. El nivel se deriva de los tokens acumulados y no se guarda.

## Seguridad

Las características de recepción y transmisión exigen un enlace cifrado y autenticado. El emparejamiento usa LE Secure Connections con código mostrado en el robot (DisplayOnly), lo que protege contra escucha pasiva y contra intermediarios. Por el enlace viajan fragmentos de transcript y los comandos completos que esperan aprobación, así que conviene mantener este requisito activo. Los bonds se persisten con `CONFIG_BT_NIMBLE_NVS_PERSIST`.

## Archivos

- `app_claude_buddy.{h,cpp}`: app Mooncake, máquina de estados y mapeo al avatar.
- `buddy_link.{h,cpp}`: transporte (framing por líneas sobre NUS, envío troceado por MTU, passkey).
- `buddy_store.h`: ajustes y estadísticas en NVS.
- `../../hal/utils/bleprph/nus_svc.{h,c}`: servicio Nordic UART sobre NimBLE.
- `../../../tools/make_buddy_icon.py`: generador del ícono del launcher.

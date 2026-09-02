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

Si un lado pierde la clave, el cifrado falla y macOS **no** vuelve a emparejar por sí solo: "Forget" en Hardware Buddy no borra el bond del sistema. Tras tres fallos seguidos el robot cambia a una dirección BLE aleatoria estática nueva (y la guarda en NVS), de modo que el Mac lo ve como un dispositivo nuevo: basta con pulsar **Connect**, elegirlo y escribir el código. Mientras tanto la burbuja informa del estado: "Mac connected, securing link...", "Pairing failed n/3", "New identity! Connect me again on the Mac".

## Estados

| Estado | Cuándo | Qué hace el robot |
|---|---|---|
| sleep | Sin conexión con el escritorio | Cara dormida, cabeza abajo, LEDs apagados. Tras cinco minutos baja el brillo de la pantalla. |
| idle | Conectado y sin actividad | Movimientos y expresiones aleatorias de reposo. |
| busy | Hay sesiones generando | Gota de sudor, movimiento lento, LEDs azules respirando. La burbuja muestra el resumen de una línea que envía el escritorio. |
| attention | Una herramienta espera permiso | Mira hacia arriba, LEDs parpadeando, botones Approve y Deny en pantalla y sonido de alerta. La cara y el color dependen del riesgo del comando (ver abajo). |
| celebrate | Sube de nivel (cada 50K tokens) | Cara feliz, meneo de cabeza, LEDs arcoíris, texto "Level N!". |
| dizzy | Se sacude el robot | Ojos en espiral durante unos segundos. |
| heart | Aprobación en menos de cinco segundos | Corazones flotando y LEDs rosa. |

### Riesgo del comando

El robot clasifica el `hint` de cada solicitud con reglas simples:

- **Peligro** (`rm -rf`, `git push --force`, `DROP TABLE`, `mkfs`, `kubectl delete`…): cara triste con sudor, LEDs rojos, texto "DANGER!". El toque de cabeza no aprueba; solo el botón.
- **Precaución** (`sudo`, `git push`, `npm publish`, `chmod`, escrituras en `~`…): cara de duda con sudor, LEDs naranja rojizo.
- **Normal**: cara de duda, LEDs naranja.

### Escalado

Si la solicitud lleva más de un minuto sin respuesta, el parpadeo se acelera y suena un recordatorio cada 30 segundos. Si la cámara no detecta a nadie, la cabeza barre a izquierda y derecha buscando al usuario.

### Mirar a quien aprueba

Mientras hay una solicitud pendiente, la cámara captura a unos 3 fps y calcula el centroide del movimiento por bandas verticales; la cabeza gira hacia donde hay movimiento. No usa detección de caras (no hay esp-dl en este build), así que reacciona a cualquier cosa que se mueva.

### Sonidos

Alerta al llegar la solicitud, recordatorio en el escalado, éxito al aprobar, vibración al denegar y aviso al subir de nivel. Se reproducen decodificando Opus directamente al códec, porque el servicio de audio de XiaoZhi no corre en modo Mooncake.

## Controles

- Tocar la pantalla abre un panel con el número de sesiones, tokens acumulados y del día, nivel, estadísticas de aprobaciones y las últimas líneas del transcript. Se cierra solo a los diez segundos o al tocarlo.
- Con una solicitud pendiente, tocar la cabeza del robot equivale a aprobar, salvo en solicitudes de peligro y durante los primeros 1.5 s (el servo al subir la cabeza puede activar el sensor).
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
- `buddy_sfx.{h,cpp}`: efectos de sonido (Ogg Opus embebidos → códec).
- `buddy_look.{h,cpp}`: seguimiento por movimiento con la cámara.
- `../../hal/utils/bleprph/nus_svc.{h,c}`: servicio Nordic UART sobre NimBLE.
- `../../../tools/make_buddy_icon.py`: generador del ícono del launcher.

# Meridian Exchange: empieza aquí

Está pensado para demostrar ingeniería de sistemas en entrevistas de prácticas: estructuras de datos, redes, contabilidad entera, persistencia, pruebas y medición.

## Primera ejecución

Instala un compilador C++20, CMake y Node.js 24. En Windows abre una terminal de Visual Studio, entra en la carpeta del proyecto y ejecuta:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
node tools/demo.mjs ./build/Release/exchange_server.exe
```

La demo verifica un cruce parcial entre dos clientes, los saldos, el reinicio con journal, el reintento sin duplicar la operación y la cancelación de las órdenes de una cuenta mediante kill switch.

## Qué cambió tras la auditoría

La v1.0.0 pasaba sus tests, y aun así una auditoría encontró defectos reales. Conviene que sepas explicar estos cambios y por qué se hicieron:

- **Lo que no se prueba bajo fallo, no se sabe.** Clasificar `EMFILE` en un test no demuestra sobrevivir a quedarse sin descriptores. Ahora una prueba lo provoca de verdad con `ulimit`.
- **Una comprobación de salud no puede hacer cola detrás del problema.** `Ping` y `Metrics` tomaban el mismo lock que el escritor, así que un `fsync` atascado los silenciaba justo cuando hacían falta.
- **Una frase en el README no es un control.** "Úsalo solo en loopback" ahora lo impone el servidor con `--allow-remote`.
- **Una cuota debe devolverse a tiempo.** Una conexión ya cerrada seguía contando hasta la siguiente vuelta del bucle, y podía rechazar a un cliente sin motivo. Apareció al revisar el test de la propia cuota.
- **Marcar algo como hecho exige evidencia.** Por eso `docs/AUDIT-FOLLOWUP.md` dice qué se hizo, qué no se construye y por qué, y deja abierto lo que sigue abierto.

## Qué debes poder explicar

1. Cómo conservan la prioridad FIFO los índices de la lista aunque se reciclen posiciones de memoria.
2. Por qué el ID de una orden y la secuencia de una petición resuelven problemas distintos.
3. Qué sucede si el proceso cae después del sync pero antes de responder.
4. Cómo se reservan efectivo e inventario, y qué se libera en una ejecución parcial.
5. Por qué cientos de nanosegundos del núcleo no implican confirmaciones persistentes igual de rápidas.
6. Cómo interpretar los descartes y la latencia del generador de carga abierta.
7. Qué recupera un snapshot del feed y qué historial se pierde cuando vence su retención, y cómo `exchange inspect` lo reconstruye desde el journal.
8. Por qué no hay un hilo escritor con su cola delante del lock, y qué mediría para cambiar de opinión.

Para una presentación de cinco minutos: ejecuta la demo, muestra una prueba de caída, compara el profiler del núcleo con el benchmark síncrono, y explica una limitación con los datos delante. El código fue generado con ayuda de Codex y Claude; estudia y modifica una parte antes de atribuirte dominio técnico. Ningún proyecto permite certificar un percentil de candidatos o garantizar una oferta.

La documentación técnica y los datos están en docs/ y bench/results/. El README incluye instrucciones para Linux y para usar clientes manuales. Las claves de ejemplo son públicas y solo sirven para la demo local.

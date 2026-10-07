# Funcionamiento de `generate_joint_detection_figure.py`

Este documento explica como funciona el script de generacion de figuras para la prueba de deteccion conjunta de enojo y hablante, y que archivos produce.

## Ubicacion de los scripts

- `outputs/generate_joint_detection_figure.py`: lanzador rapido ubicado junto al CSV de resultados. Si se ejecuta sin argumentos, usa automaticamente:
  - CSV de entrada: `outputs/joint_detection_results.csv`
  - carpeta de salida: `outputs/figures`
- `experiments/generate_joint_detection_figure.py`: script principal. Contiene toda la logica de validacion, calculo de metricas, graficacion y exportacion del JSON.
- `experiments/create_joint_detection_template.py`: script relacionado que crea la plantilla CSV de 256 filas para llenar durante las pruebas.

## Como ejecutarlo

Desde la raiz del proyecto se puede usar el lanzador:

```bash
python outputs/generate_joint_detection_figure.py
```

Tambien se puede llamar al modulo principal indicando rutas manualmente:

```bash
python -m experiments.generate_joint_detection_figure \
  --csv outputs/joint_detection_results.csv \
  --output-dir outputs/figures
```

## Entrada esperada

El archivo de entrada es `outputs/joint_detection_results.csv`. Cada fila representa una medicion del ESP32 en un punto espacial especifico, para un hablante y un tipo de frase.

Columnas requeridas:

- `line_id`: linea radial del protocolo, de `1` a `8`.
- `distance_cm`: distancia desde el dispositivo, una de `50`, `100`, `150` o `200`.
- `angle_deg`: angulo asociado a la linea. Debe cumplir `(line_id - 1) * 45`.
- `speaker`: hablante real: `papa`, `mama`, `hijo` o `hija`.
- `phrase_type`: tipo de frase: `neutral` o `angry`.
- `anger_detected`: salida del sistema para enojo, `0` o `1`.

Columnas opcionales, pero recomendadas:

- `predicted_speaker`: hablante reportado por el ESP32. Puede ser `papa`, `mama`, `hijo`, `hija` o marcas como `inseguro`, `parcial` o `confusion`.
- `speaker_correct`: `1` si el hablante fue identificado correctamente, `0` si no.
- `notes`: observaciones de la toma, por ejemplo ruido, distancia, confusion o necesidad de repetir la frase.

El protocolo completo espera:

```text
8 lineas * 4 distancias * 4 hablantes * 2 frases = 256 filas
```

Si el CSV no tiene 256 filas, el script no se detiene automaticamente, pero muestra una advertencia.

## Flujo interno del script

1. Lee argumentos de consola con `--csv` y `--output-dir`.
2. Crea la carpeta de salida si no existe.
3. Carga el CSV con `pandas`.
4. Valida que existan las columnas requeridas.
5. Normaliza texto:
   - convierte a minusculas;
   - quita espacios;
   - elimina acentos;
   - acepta alias como `padre -> papa` y `madre -> mama`.
6. Valida valores esperados:
   - lineas `1..8`;
   - distancias `50, 100, 150, 200`;
   - angulos `0, 45, 90, 135, 180, 225, 270, 315`;
   - hablantes y tipos de frase validos.
7. Valida que `anger_detected` sea binario (`0` o `1`).
8. Calcula `speaker_correct` si esa columna esta vacia, usando `predicted_speaker == speaker`.
9. Calcula las metricas por fila.
10. Agrupa resultados por punto espacial.
11. Genera tres mapas polares en PDF.
12. Escribe el resumen numerico en `joint_detection_stats.json`.

## Metricas calculadas

### `expected_anger`

Valor esperado segun el tipo de frase:

- `neutral` espera `0`.
- `angry` espera `1`.

### `anger_correct`

Indica si la deteccion de enojo fue correcta:

```text
anger_correct = anger_detected == expected_anger
```

### `speaker_accuracy`

Usa directamente `speaker_correct`. Si `speaker_correct` esta vacio, el script intenta calcularlo comparando `predicted_speaker` contra `speaker`.

### `joint_accuracy`

Es la metrica conjunta. Solo vale `1` cuando el sistema acerto enojo y hablante en la misma medicion:

```text
joint_accuracy = anger_correct == 1 AND speaker_correct == 1
```

## Archivos que genera

Todos se escriben dentro de `outputs/figures`.

### `fig_joint_anger_polar.pdf`

Mapa polar de exactitud de deteccion de enojo.

Cada celda representa un punto espacial compuesto por una linea radial y una distancia. El color indica el promedio de `anger_correct` en ese punto.

Sirve para ver en que posiciones el sistema detecta mejor o peor la emocion de enojo.

### `fig_joint_speaker_polar.pdf`

Mapa polar de exactitud de identificacion de hablante.

Cada celda muestra el promedio de `speaker_accuracy` en ese punto. Es util para observar si la distancia o el angulo afectan la identificacion de `papa`, `mama`, `hijo` o `hija`.

### `fig_joint_combined_polar.pdf`

Mapa polar de exactitud conjunta.

Cada celda muestra el promedio de `joint_accuracy`. Esta es la figura mas estricta, porque exige que el sistema acierte al mismo tiempo:

- si habia enojo o no;
- quien estaba hablando.

### `joint_detection_stats.json`

Archivo JSON con las metricas resumidas para reporte, analisis o captions de figuras.

Contiene:

- `overall`: metricas globales de todas las muestras.
- `per_distance_cm`: metricas agrupadas por distancia.
- `per_line`: metricas agrupadas por linea radial y angulo.
- `per_point`: metricas de cada combinacion `line_id + distance_cm`.
- `speaker_confusion`: tabla de confusion entre hablante real y hablante predicho.

En la ejecucion actual, el JSON resume `256` muestras con estos valores globales:

- `anger_accuracy`: `0.9453` (94.53 %)
- `speaker_accuracy`: `0.8594` (85.94 %)
- `joint_accuracy`: `0.8281` (82.81 %)
- `speaker_error_rate`: `0.1406` (14.06 %)
- `anger_false_positive_rate_neutral`: `0.0234` (2.34 %)
- `anger_miss_rate_angry`: `0.0859` (8.59 %)

## Interpretacion de las figuras

Los mapas son polares:

- el centro representa el ESP32;
- cada direccion angular corresponde a una linea `L1..L8`;
- cada anillo radial representa una distancia (`50`, `100`, `150`, `200` cm);
- la escala de color va de `0 %` a `100 %` de accuracy.

El script fija `L1` en la parte superior del grafico y avanza en sentido horario, igual que el protocolo espacial.

## Errores comunes que el script detecta

- Columnas requeridas faltantes.
- Valores vacios o no numericos en `anger_detected`.
- Valores distintos de `0` o `1` en columnas binarias.
- Angulos que no coinciden con la formula `(line_id - 1) * 45`.
- Hablantes, frases, lineas o distancias fuera del protocolo.
- Filas sin `speaker_correct` y sin `predicted_speaker`, porque no se puede calcular la exactitud del hablante.

## Relacion con el CSV

El script no modifica `outputs/joint_detection_results.csv`; solo lo lee. Si se corrigen datos en el CSV, hay que ejecutar nuevamente el generador para actualizar los PDF y el JSON.


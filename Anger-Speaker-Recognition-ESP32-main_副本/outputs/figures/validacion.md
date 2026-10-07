# Validacion 

Este documento resume la validacion experimental del sistema V7 ya desplegado en el ESP32-S3 N16R8. Esta validacion no forma parte del entrenamiento ni modifica los pesos del modelo; su objetivo es medir el comportamiento real del pipeline completo bajo condiciones domesticas, incluyendo microfono I2S, VAD, MFCC, Mini Xi-vector, inferencia TFLite Micro, umbrales y decision final.

## Diferencia con la evaluacion del notebook

La evaluacion del notebook mide el modelo sobre datos preparados fuera del ESP32. En cambio, esta validacion mide el sistema completo funcionando en el hardware real.

| Tipo de evaluacion | Donde ocurre | Que mide |
| --- | --- | --- |
| Evaluacion de notebook | Python / dataset preparado | Capacidad del modelo sobre datos procesados fuera del ESP32 |
| Validacion post-despliegue | ESP32-S3 en entorno domestico | Rendimiento real del sistema completo con audio capturado en vivo |

## Archivos relacionados

| Archivo | Funcion |
| --- | --- |
| `PROTOCOL_joint_detection.md` | Protocolo experimental usado para la prueba espacial |
| `outputs/joint_detection_results.csv` | Planilla con las 256 mediciones registradas |
| `outputs/generate_joint_detection_figure.py` | Lanzador que genera figuras y estadisticas |
| `outputs/figures/joint_detection_stats.json` | Resumen numerico de la validacion |
| `outputs/figures/fig_joint_anger_polar.pdf` | Mapa polar de deteccion de enojo |
| `outputs/figures/fig_joint_speaker_polar.pdf` | Mapa polar de identificacion de hablante |
| `outputs/figures/fig_joint_combined_polar.pdf` | Mapa polar de exactitud conjunta |

## Protocolo resumido

Cada fila del CSV representa una medicion del sistema en un punto espacial definido por linea radial, distancia, hablante y tipo de frase.

El protocolo completo contiene:

```text
8 lineas x 4 distancias x 4 hablantes x 2 frases = 256 muestras
```

Las distancias evaluadas son:

```text
50 cm, 100 cm, 150 cm, 200 cm
```

Las lineas radiales corresponden a:

```text
0, 45, 90, 135, 180, 225, 270, 315 grados
```

## Metricas

| Metrica | Significado |
| --- | --- |
| `anger_accuracy` | Proporcion de frases donde el sistema acerto si habia enojo o no |
| `speaker_accuracy` | Proporcion de frases donde el sistema identifico correctamente al hablante |
| `joint_accuracy` | Proporcion de frases donde acerto enojo y hablante en la misma medicion |
| `speaker_error_rate` | Proporcion global de error al identificar hablante |
| `anger_false_positive_rate_neutral` | Frases neutrales marcadas incorrectamente como enojo |
| `anger_miss_rate_angry` | Frases enojadas donde no se detecto enojo |

El `joint_accuracy` es la metrica mas estricta porque solo cuenta como acierto cuando ambas ramas aciertan simultaneamente:

```text
joint_accuracy = anger_correct AND speaker_correct
```

## Generacion de resultados

Para generar las figuras y el resumen numerico:

```bash
python outputs/generate_joint_detection_figure.py
```

Ese script usa automaticamente:

```text
CSV de entrada: outputs/joint_detection_results.csv
Salida: outputs/figures
```

## Resultados globales actuales

| Metrica global | Resultado |
| --- | ---: |
| Muestras totales | 256 |
| `anger_accuracy` | 94.53 % |
| `speaker_accuracy` | 85.94 % |
| `joint_accuracy` | 82.81 % |
| `speaker_error_rate` | 14.06 % |
| `anger_false_positive_rate_neutral` | 2.34 % |
| `anger_miss_rate_angry` | 8.59 % |

## Resultados por distancia

El rendimiento conjunto muestra una degradacion esperada conforme aumenta la distancia al microfono.

| Distancia | `anger_accuracy` | `speaker_accuracy` | `joint_accuracy` |
| ---: | ---: | ---: | ---: |
| 50 cm | 98.44 % | 96.88 % | 96.88 % |
| 100 cm | 93.75 % | 92.19 % | 87.50 % |
| 150 cm | 93.75 % | 89.06 % | 84.38 % |
| 200 cm | 92.19 % | 65.62 % | 62.50 % |

## Interpretacion

Los resultados muestran que el sistema conserva buena deteccion de enojo incluso a mayor distancia, pero la identificacion de hablante se vuelve mas sensible a distancia, ruido y orientacion.

Por eso esta validacion complementa las metricas del notebook: permite observar el comportamiento real del sistema completo, no solo del modelo aislado.

El archivo `joint_detection_stats.json` incluye una seccion `_metadata` que documenta el significado de sus campos. Esta seccion funciona como comentario compatible con JSON, ya que el formato JSON no permite comentarios `//` o `/* */`.


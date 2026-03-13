// Acá van los comentarios para el avance en pruebas y hallazgos importantes en el diseño de plug-in

// Alejandro -- Por acá comparto link hacia el google sheets con las mediciones de los Guide Tracks: https://docs.google.com/spreadsheets/d/1G2toHNcKeqyDvQZDAT9z7O395x12XvxKMoVS8KfEI7Q/edit?usp=sharing
// Alejandro -- V1.0.0.0 del plugin falló. Le sube el volumen al audio sin tener makeup gain.
// Alejandro -- v1.0.1 del plugin funcionó bien. Obtuvimos los siguientes resultados:
        Tres presets que funcionan para todos los escenarios:
        1) Caso Bajo LRA, Bajo Integrado
        2) Caso Alto LRA, Medio Integrado
        3) Caso Alto LRA, Alto Integrado
  Se añadirán estos presets para la versión 1.0.2

--------------------------------------------------------------------------------------------------------------------------------

// Alejandro -- Setting de los presets:
        1) Alto LRA, Alto Integrado Suavecito: 
                Comp A -21 Thresh, 1.5 Ratio, 5.0ms Attack, 125ms Release 
                Comp B -12 Thresh, 2.5 Ratio, 5.0ms Attack, 125ms Release
                Comp C -6 Thresh, 3.0 Ratio, 5.0ms Attack, 125ms Release
                Make Up +4dBs
        2) Alto LRA, Alto Integrado Calientito: 
                Comp A -21 Thresh, 1.5 Ratio, 5.0ms Attack, 125ms Release 
                Comp B -12 Thresh, 2.5 Ratio, 5.0ms Attack, 125ms Release
                Comp C -6 Thresh, 3.0 Ratio, 5.0ms Attack, 125ms Release
                Make Up +9dBs
        3) Alto LRA, Medio Integrado:
                Comp A -24 Thresh, 2.5 Ratio, 5.0ms Attack, 125ms Release 
                Comp B -12 Thresh, 5.0 Ratio, 5.0ms Attack, 125ms Release
                Comp C -3 Thresh, 8.0 Ratio, 5.0ms Attack, 125ms Release
                Make Up +10dBs
        4) Alto LRA, Bajo Integrado
                Comp A -28 Thresh, 3.0 Ratio, 5.0ms Attack, 125ms Release 
                Comp B -20 Thresh, 6.0 Ratio, 5.0ms Attack, 125ms Release
                Comp C -10 Thresh, 8.0 Ratio, 5.0ms Attack, 125ms Release
                Make Up +14dBs
        5) Bajo LRA, Alto Integrado
                Comp A -21 Thresh, 1.2 Ratio, 5.0ms Attack, 125ms Release 
                Comp B -12 Thresh, 2.5 Ratio, 5.0ms Attack, 125ms Release
                Comp C -3 Thresh, 3.0 Ratio, 5.0ms Attack, 125ms Release
                Make Up +10dBs

--------------------------------------------------------------------------------------------------------------------------------


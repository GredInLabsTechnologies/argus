# Firma del driver `argus` (monitor virtual IddCx)

> Estado y opciones para que el driver cargue **sin test-signing en cualquier PC**.
> Escrito 2026-06-18 tras diagnosticar por qué argus caía a MIRROR (no EXTEND).

## Estado actual (el problema)

- El binario embebido (`mttvdd.cat`/`.dll`/`.inf`) está **test-signed con `WDKTestCert` local**
  (`CN="WDKTestCert shilo"`). Eso **solo carga con `bcdedit /set testsigning on` + reinicio**.
- Sin testsigning, Windows no carga el `.sys`/UMDF → el devnode `SWD\MttVDD\MttVDD_Istro` queda
  **"Desconectado"** → el broker (`istro-svc`) falla `enable_display` (CONFIGRET 5) → captura el
  monitor PRINCIPAL en **MIRROR** en vez del virtual en EXTEND.
- El CI (`.github/workflows/ci-validation.yml`) **compilaba pero NO firmaba**. Ahora tiene un paso de
  firma SignPath **condicional** (ver abajo), inactivo hasta configurar los secrets.

## ✅ Confirmado: la firma OV (SignPath) BASTA para este driver en x64

El plan de `Istro/docs/argus-driver-build.md` era **correcto**: *"user-mode IddCx → basta firma OV
Authenticode (SignPath Foundation gratis), no EV/atestación"*. Verificado con investigación + evidencia
del upstream (2026-06-18):

- **Los drivers UMDF / user-mode NO requieren code-integrity signing** (eso es exclusivo de los
  drivers **kernel-mode**, que sí exigen attestation/EV). IddCx es user-mode → solo necesita que el
  **paquete** esté firmado con Authenticode (integridad + trazabilidad), no la cadena de drivers de MS.
- **Evidencia decisiva:** el **upstream `VirtualDrivers/Virtual-Display-Driver` (del que argus es fork)
  ya firma con SignPath Foundation**, y sus releases x64 **cargan SIN testsigning**. Su README solo
  menciona testsigning para **ARM64 en Windows 11 24H2+** — no para x64.
- Referencias: Microsoft Learn *Do UMDF drivers require signing?* (no code-integrity); README del
  upstream (SignPath, sin testsigning en x64).

**Conclusión:** SignPath OV gratis es **suficiente** para que argus cargue sin testsigning en el host
x64. No hace falta EV ni Partner Center. (Único matiz: si algún día hay host **ARM64 en 24H2+**, ese
sí podría requerir testsigning — irrelevante para el host x64 actual con RTX 3060.)

## Vía elegida: SignPath Foundation (OV, gratis) — ya montada en el CI

- Gratis para OSS (repo `argus` es público, MIT, mantenido → califica). **Aprobación manual de
  SignPath (días) la primera vez.**
- Re-firma el `.cat` con publisher real ("Gredin Labs") → carga sin testsigning **y** satisface el
  gate del broker (`istro-svc` rechaza el `WDKTestCert`).
- Es exactamente lo que hace el upstream. Sin coste.

### Alternativas (solo si SignPath se complicara o para ARM64)
- **Azure Trusted Signing** (~10 $/mes): firma en CI, requiere verificación de organización.
- **Attestation / Partner Center + EV cert** (~250-600 €/año): la vía de los drivers **kernel-mode**;
  **no necesaria** para este driver user-mode. Solo relevante si el modelo cambiara a kernel-mode.

### Estado de desarrollo actual
- Test-signing (`bcdedit /set testsigning on` + reinicio) ya activado en el PC de dev para validar
  EXTEND **hoy**, mientras se tramita SignPath. No distribuible — se sustituye por la firma SignPath.

## Setup de la opción C (SignPath) — ya montado en el CI, falta tu parte

El workflow ya tiene el paso `Sign driver catalog (SignPath)`, **condicional** a que existan los
secrets (si no, el CI compila como siempre). Para activarlo:

1. **Solicita el proyecto en SignPath Foundation:** <https://signpath.org> → "Apply for OSS".
   Indica el repo `https://github.com/GredInLabsTechnologies/argus`. Esperar aprobación (días).
2. En SignPath, crea: el **project** (slug `argus`), una **artifact-configuration** que firme el
   `mttvdd.cat` dentro del artefacto subido (slug `driver-cat`), y dos **signing-policies**
   (`test-signing`, `release-signing`).
3. En GitHub (repo argus) → Settings → Secrets and variables → Actions:
   - **Secret** `SIGNPATH_API_TOKEN` = el CI token de SignPath.
   - **Variable** `SIGNPATH_ORGANIZATION_ID` = el GUID de tu organización en SignPath.
4. Push a `main` → el CI compila, sube el `.cat` sin firmar, SignPath lo firma (tras aprobación
   manual de cada release) y publica el artefacto `VDD-…-SIGNED`.

## Recomendación

- **Hoy:** validar EXTEND con testsigning (ya activado) — confirma que argus funciona de verdad.
- **Producto (gratis):** **SignPath Foundation OV** — el CI ya lo tiene montado; falta dar de alta el
  proyecto en SignPath y configurar 2 valores en GitHub (ver setup arriba). Es lo que hace el upstream
  y carga sin testsigning en x64. **Cero coste.**
- EV/attestation NO hace falta para este driver (es user-mode). Se descarta salvo cambio de modelo.

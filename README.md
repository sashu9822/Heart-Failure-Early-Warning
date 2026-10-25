# SteadyStep — Alert Logic, Reproducible Program & Results

## What this package does

This package runs the current **provisional SteadyStep rule logic** on a synthetic longitudinal dataset with the three intended measurement fields:

1. Body weight (kg)
2. Resting heart rate (beats/minute)
3. Foot-to-foot bioelectrical impedance (ohms)

It writes daily decisions, patient-level event-detection results, a held-out performance chart, a confusion matrix and machine-readable metrics. The rule engine only receives the measurement file. Synthetic labels are merged **after** predictions to evaluate the output; they are never rule inputs.

> **Critical:** every value and outcome label in this package is synthetic. The results quantify agreement with a hand-designed simulator, not clinical accuracy, and do not establish that SteadyStep can diagnose or reliably detect worsening heart failure.

## Run it

Python 3.10+ is recommended.

```bash
python -m venv .venv
# Windows PowerShell: .venv\Scripts\Activate.ps1
# macOS/Linux: source .venv/bin/activate
pip install -r requirements.txt
python scripts/run_validation.py
```

Results are written to `results/`. The script is deterministic with the included data and split seed.

## Input files and leakage prevention

- `data/measurements_only.csv` — **only input to the rule engine**. Contains 27,000 daily rows for 300 virtual patients over 90 simulated days. It has measurements, day/date and signal-quality flags only.
- `data/daily_evaluation_labels_ONLY.csv` — synthetic worsening-period and event labels. Evaluation only; never passed to the rule engine.
- `data/event_labels_ONLY.csv` — synthetic onset/event-day labels. Used only after predictions to determine whether an alert occurred during the simulated worsening interval.

The program splits whole patients into development and held-out test sets (70/30, stratified by whether a synthetic event was assigned). A patient’s days never appear in both sets. It does not tune thresholds on the held-out set.

## Exact decision logic

### 1. Signal validity

A weight observation is accepted only when its quality flag is `valid`, its value is finite, and it lies in the broad engineering sanity range 20–250 kg. Resting HR is accepted only when its quality flag is `valid`, its value is finite, and it lies in 40–180 bpm. Impedance is recorded and sanity-checked (100–2000 ohms) but **is not used to trigger alerts**, because the current physical impedance front end has not been verified. These sanity ranges are software filters, not clinical reference ranges.

### 2. Personal baseline

Weight and resting HR each have their own baseline. The baseline is the **median of the first 7 valid observations** for that channel for that person. Until both baselines are established, the state is `CALIBRATING`, unless a persistent signal already meets an alert rule. The median reduces sensitivity to one unusually high or low starting measurement. The baseline is a provisional engineering choice and may be contaminated if the person is already worsening during calibration.

### 3. Per-session thresholds

For each valid observation after that channel's baseline is ready:

- Weight threshold is exceeded when current weight is **at least 2% above** the personal weight baseline: `weight >= baseline_weight × 1.02`.
- Resting-HR threshold is exceeded when current resting HR is **at least 15% above** the personal HR baseline: `HR >= baseline_HR × 1.15`.

These percentages are exploratory placeholders used to demonstrate the pipeline. They are **not clinician-approved thresholds** and must not be presented as medical standards.

### 4. Persistence filter

For each channel independently, the rule looks at the last 3 **valid evaluated sessions** after baseline calibration. A channel is persistent if its threshold was exceeded in at least 2 of those 3 sessions. Invalid sessions do not count as threshold exceedances and do not enter that channel's persistence history.

### 5. Alert states

| Output state | Exact basis | Meaning |
|---|---|---|
| `ELEVATED_PATTERN` | Both weight and resting-HR signals meet their persistence rule | Two configured trend rules are present; review is warranted in the prototype workflow. |
| `WATCH` | Either weight or resting-HR signal meets its persistence rule | One configured trend rule is present; review is warranted in the prototype workflow. |
| `CALIBRATING` | Fewer than 7 valid baseline observations for weight or HR, and no persistent alert | Not enough personal baseline data yet. |
| `INSUFFICIENT_DATA` | No valid weight/HR channel can be assessed, or no threshold fires while impedance remains unverified/unavailable | The system cannot make a sufficiently complete assessment. This is **not normal**. |
| `NO_RULE_TRIGGER` | Both baselines are ready, current weight and HR are valid, impedance is verified and valid, and neither persistent rule fires | Only means the configured rules did not trigger. It does **not** prove the person is medically normal. In this version `IMPEDANCE_VERIFIED = False`, so this state is intentionally not expected during routine replay. |

**What counts as “normal”?** The program does not medically classify a patient as normal. For evaluation only, the synthetic generator labels days outside its simulated worsening interval as “synthetic normal days.” A `NO_RULE_TRIGGER` state is a rule-engine result, not a clinical diagnosis or an all-clear. Missing, poor-quality, calibrating, or unverified data must never be described as normal.

### 6. Why impedance is not an alert input

The dataset contains foot-to-foot impedance, but the prototype's analogue front end and measurement safety/accuracy have not been verified. Foot-to-foot impedance is not interchangeable with validated thoracic impedance. The program therefore records it, checks its signal metadata, and marks the channel as unverified; it does not invent an impedance trend threshold. Once the circuit is validated, impedance logic should be designed and evaluated separately before being added.

## How the reported results are calculated

The report focuses on the untouched held-out patients. After predictions are created, synthetic evaluation labels are joined to calculate:

- **Patient-level event sensitivity:** fraction of event-labelled test patients with at least one `WATCH`/`ELEVATED_PATTERN` alert between their simulated worsening-onset day and simulated event day.
- **Median lead time:** median number of simulated days from the first alert in that interval to the simulated event, among detected event cases only.
- **Worsening-day sensitivity:** fraction of synthetic worsening days after the first 7 calibration days that have a warning state.
- **Specificity on weight/HR-evaluable synthetic normal days:** fraction of synthetic normal days with no weight/HR-rule warning, restricted to days after baseline calibration where both weight and HR are valid. This evaluates only the two currently used channels; it does not validate impedance or the complete three-measurement system.
- **Accuracy on weight/HR-evaluable days only:** binary agreement between warning/no-warning and synthetic worsening/not-worsening labels, restricted to post-calibration days with valid weight and HR. This is rule agreement with synthetic labels, not clinical accuracy.
- **False alert episodes per patient-month:** starts of warning episodes on synthetic normal days divided by total post-calibration patient-days expressed in 30.4375-day months. This all-days denominator is descriptive and should be interpreted alongside coverage.
- **Coverage:** fraction of post-calibration days with valid weight and HR and an established baseline, eligible for these two-channel rule metrics. The alert-state output can still be `INSUFFICIENT_DATA` because impedance is unverified; that must not be read as normal.

These are not interchangeable. High specificity on a tiny subset, or high overall accuracy in a dataset with many normal days, can hide poor event sensitivity. Always report the event sensitivity, specificity, lead time, false alerts and coverage together.

## Rule-engine unit tests

Run the small deterministic test suite with:

```bash
python -m unittest discover -s tests -v
```

The tests exercise weight-only warning, combined warning, unverified-impedance handling, bad-quality data, and prevention of evaluation-label leakage. They verify code behavior—not medical validity.

## Files generated in `results/`

- `metrics_summary.json` — full metrics, parameters, confusion matrix and limitations.
- `held_out_performance.png` — held-out synthetic performance chart.
- `held_out_daily_predictions_and_evaluation_labels.csv` — daily outputs plus labels added after prediction, for audit.
- `held_out_event_detection_results.csv` — event-level detection and lead-time results.
- `development_daily_predictions_and_evaluation_labels.csv` — development outputs for audit.
- `development_event_detection_results.csv` — development event results.
- `patient_level_split.csv` — deterministic patient-level split.

## Medical basis and validation status — read before interpreting results

**Medical validation status: NOT clinically validated.** This package has engineering verification on hand-written test cases and a reproducible replay against synthetic labels. It has not been validated on real patients, clinician-adjudicated worsening events, hospital admissions, or a prospective clinical study. Therefore it cannot honestly claim medically validated accuracy, diagnostic performance, or that a `NO_RULE_TRIGGER` state means a patient is safe or normal.

### What medical evidence supports—and what it does not

- **Daily weight tracking:** the American Heart Association advises people with heart failure to weigh themselves consistently and contact their healthcare professional for rapid weight gain; its public guidance gives examples such as 2–3 lb in one day for several days or 5 lb in a week, while emphasizing individual care-team instructions. Source: https://www.heart.org/en/health-topics/heart-failure/treatment-options-for-heart-failure/lifestyle-changes-for-heart-failure
- **Our +2% personal-baseline weight rule is not that guidance.** It is a prototype engineering threshold expressed as a percentage of an individual's baseline. It is not clinician-approved, is not interchangeable with the AHA's absolute weight-change examples, and needs prospective clinical assessment before use.
- **Resting HR +15%:** no clinical validation is provided here for this relative threshold as a heart-failure deterioration detector. It is a provisional engineering parameter, not a guideline threshold.
- **Foot-to-foot impedance:** although the dataset contains impedance values, this package does not use them to trigger alerts. The physical front end, electrical safety, measurement repeatability, and clinical association have not been established. Foot-to-foot impedance must not be represented as equivalent to validated thoracic impedance.
- **Clinical software evaluation:** the IMDRF SaMD clinical-evaluation framework distinguishes (1) valid clinical association, (2) analytical/technical validation, and (3) clinical validation in the intended population and intended use. This package only addresses limited software-rule verification and synthetic analytical replay; it does not establish clinical validation. Source: https://www.fda.gov/files/medical%20devices/published/Software-as-a-Medical-Device-%28SAMD%29--Clinical-Evaluation---Guidance-for-Industry-and-Food-and-Drug-Administration-Staff.pdf

### What would be needed for medical validation

1. **Sensor/analytical validation:** compare platform weight against a calibrated scale; compare resting HR against an appropriate reference device/ECG under the intended conditions; characterize repeatability, motion artefacts, missingness, calibration drift, and error bounds. Validate the impedance electronics and safe excitation separately before including impedance in the rule engine.
2. **Clinical protocol:** define intended users, care setting, target outcome, alert action, and event timing with heart-failure clinicians. Pre-register clinically justified thresholds and handling of medication changes, arrhythmia, activity, and comorbidities. Do not tune and evaluate on the same patients.
3. **Real-world cohort:** prospectively collect consented data from the target population, with ethics/privacy approval as applicable. Compare alerts against clinician-adjudicated deterioration outcomes and/or clearly defined clinical endpoints. Use patient-level (and preferably temporal/site-level) separation between development and external test cohorts.
4. **Report uncertainty and harms:** provide sensitivity, specificity, PPV, NPV, lead time, false alerts per patient-month, coverage, subgroup performance, confidence intervals, and failure/missing-data analysis. Evaluate false reassurance and alert fatigue—not accuracy alone.
5. **Independent review:** obtain clinical, safety, regulatory, and data-governance review before any real patient-facing deployment. Until then, use only as a hackathon/demo prototype and never for treatment decisions.

The synthetic test labels in this package were created by a simulator, not by clinicians or medical records. Thus even perfect agreement with those labels would only show that the program reproduces the simulator's assumptions; it would not prove medical effectiveness.

## Engineering and clinical limitations

1. This is a synthetic simulation, not a real patient dataset. Generator assumptions may make the rule look better or worse than real-world performance.
2. Thresholds (+2% weight, +15% HR) are not clinically validated.
3. Impedance is not used for alerts; the three-measurement system is therefore **not yet validated as a three-signal detector**.
4. These outputs do not validate load cells, HX711 acquisition, BCG heart-rate extraction, impedance electronics, speaker, GSM/SMS delivery or real-time embedded behavior.
5. A no-alert state cannot rule out worsening heart failure. Do not use this prototype to make treatment decisions or delay clinical care. Alerts should prompt human review under a clinician-approved protocol.
6. The separate BIDMC CHF database contains ECG, not this device's foot-to-foot impedance or daily weight trajectories. It cannot validate the full alert logic. No BIDMC benchmark result is claimed in this package.

## Suggested hackathon wording

“SteadyStep demonstrates a transparent, patient-baseline trend-rule prototype tested on 300 simulated longitudinal profiles with a patient-level held-out split. Current rules monitor persistent changes in weight and resting heart rate; impedance is logged but excluded from alerting until the measurement front end is verified. Reported metrics are synthetic engineering results, not clinical validation.”

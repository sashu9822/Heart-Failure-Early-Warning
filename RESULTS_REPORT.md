# SteadyStep — Held-out synthetic replay results

## Test design
- 300 synthetic virtual patients, 90 days each (27,000 measurement rows).
- Patient-level split: 210 development patients and 90 held-out test patients; patients are not split across sets.
- Rule engine input: measurements only. Synthetic labels are joined after predictions.
- Fixed thresholds: weight +2%, resting HR +15%, persistence in at least 2 of last 3 valid evaluated sessions.
- Foot-to-foot impedance is present in the dataset but excluded from alert decisions because its physical measurement front end is unverified.

## Held-out results
- Event cases: 23
- Events with at least one warning during synthetic worsening: 9/23 (39.1%)
- Median lead time among caught events: 2.0 simulated days
- Worsening-day sensitivity: 24.2%
- Specificity on valid weight/HR-evaluable synthetic normal days: 93.9%
- Binary accuracy on valid weight/HR-evaluable days: 92.4%
- Weight/HR-evaluable coverage after baseline period: 87.1%
- False alert days on evaluable synthetic normal days: 389
- False warning episode starts on synthetic normal days: 105
- False warning episodes per patient-month: 0.428

### Evaluable-day confusion matrix
These counts are for days after the 7-observation calibration period with valid weight and HR, compared with synthetic worsening labels; they do not evaluate impedance or clinical outcomes.

| | Synthetic worsening | Synthetic normal day |
|---|---:|---:|
| Warning | TP = 34 | FP = 389 |
| No warning | FN = 107 | TN = 5980 |

## Interpretation and limitations
The 92.4% accuracy is **binary agreement with synthetic labels on evaluable weight/HR days**, not clinical accuracy. Event sensitivity is 39.1% in this held-out subset, so most event-labelled cases were not caught during the simulated worsening interval. High binary accuracy should not obscure that low event sensitivity, the false-alert burden, or the fact that impedance is not used. This is a demonstration of transparent rule execution, not a validated heart-failure detector.

The output states `INSUFFICIENT_DATA` for non-warning sessions because the impedance pathway remains unverified. This is deliberate: a no-alert result is not treated as medically normal. Synthetic “normal day” labels mean only that the simulator did not mark a worsening interval on that day.

See `README.md` for exact rule thresholds, the difference between a no-trigger state and medical normality, metric definitions, run instructions and limitations.


## Medical validation statement
This is **not medically validated**. The inputs and worsening/event labels are synthetic and simulator-generated. The +2% weight and +15% resting-HR thresholds are engineering placeholders, not clinician-approved thresholds. Foot-to-foot impedance is excluded from alerting. The American Heart Association's public heart-failure guidance supports consistent daily weight tracking and contacting a care team for rapid weight gain, but it does not validate this program's percentage threshold or its combined logic. See the README for source links and a proposed clinical validation plan.

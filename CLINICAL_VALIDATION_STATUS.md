# Clinical validation status

**Status: not clinically validated; demonstration only.**

The current package establishes that the rule program runs reproducibly on the included synthetic dataset and passes five deterministic unit tests. It does not establish that the rules detect real heart-failure worsening safely or accurately.

- The dataset is entirely synthetic and contains no real patient measurements or clinician-adjudicated outcomes.
- The reported 92.4% is binary agreement with synthetic labels on evaluable weight/HR days, not clinical accuracy.
- Held-out event sensitivity is 9/23 (39.1%) in the synthetic test subset. This is low and means the current logic misses most simulated events in that evaluation.
- The median lead time of 2 days applies only to the nine detected synthetic events.
- Weight +2% and resting HR +15% are exploratory engineering thresholds, not validated medical thresholds.
- Foot-to-foot impedance is logged but does not contribute to alerts; its hardware and clinical interpretation are unverified.
- `INSUFFICIENT_DATA` and `CALIBRATING` are not normal. `NO_RULE_TRIGGER` would only mean no configured rule fired, not that a patient is medically safe.

## Evidence needed before clinical claims

1. Bench validation of weight, HR, and impedance measurement performance against appropriate reference instruments.
2. Clinician-defined intended use, endpoints, alert actions, and pre-specified thresholds.
3. Prospective data from the target heart-failure population, with ethics/privacy review as applicable and independent outcome adjudication.
4. Patient-level development/test separation, ideally with an external cohort/site.
5. Sensitivity, specificity, PPV, NPV, lead time, false alerts per patient-month, coverage, confidence intervals, subgroup analysis, and safety/false-reassurance analysis.
6. Independent clinical and regulatory review before patient-facing deployment.

## References

- American Heart Association, *Lifestyle Changes for Heart Failure*: https://www.heart.org/en/health-topics/heart-failure/treatment-options-for-heart-failure/lifestyle-changes-for-heart-failure
- IMDRF/FDA, *Software as a Medical Device (SaMD): Clinical Evaluation*: https://www.fda.gov/files/medical%20devices/published/Software-as-a-Medical-Device-%28SAMD%29--Clinical-Evaluation---Guidance-for-Industry-and-Food-and-Drug-Administration-Staff.pdf

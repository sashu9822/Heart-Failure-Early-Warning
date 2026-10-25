#!/usr/bin/env python3
"""Reproducible patient-level evaluation of SteadyStep's provisional rules.

The rule engine receives ONLY data/measurements_only.csv. Evaluation labels are
loaded only after predictions are produced. All supplied measurements and labels
are synthetic; this is not clinical validation.
"""
from pathlib import Path
import json
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"
OUT = ROOT / "results"
SEED = 20261025
BASELINE_N = 7
WINDOW = 3
HITS = 2
WEIGHT_RISE_PCT = 2.0
HR_RISE_PCT = 15.0
IMPEDANCE_VERIFIED = False  # physical front-end not yet verified

REQUIRED = {
    "patient_id", "day_index_0_based", "date", "weight_kg", "resting_hr_bpm",
    "foot_impedance_ohm", "weight_signal_quality", "heart_rate_signal_quality",
    "impedance_signal_quality"
}

def make_patient_split(events):
    """Deterministic 70/30 split, stratified by synthetic event assignment, by patient."""
    rng = np.random.default_rng(SEED)
    assignments = {}
    has_event = events["simulated_event_day"].notna()
    for frame in (events[has_event], events[~has_event]):
        ids = frame["patient_id"].astype(str).to_numpy().copy()
        rng.shuffle(ids)
        n_dev = int(round(0.70 * len(ids)))
        for pid in ids[:n_dev]: assignments[pid] = "development"
        for pid in ids[n_dev:]: assignments[pid] = "held_out_test"
    return pd.DataFrame({"patient_id": list(assignments), "split": list(assignments.values())}).sort_values("patient_id")

def run_rules(measurements):
    """Return predictions from measurement columns only; labels are not accepted here."""
    missing = REQUIRED - set(measurements.columns)
    if missing: raise ValueError(f"Missing measurement columns: {sorted(missing)}")
    extra_forbidden = {c for c in measurements.columns if "label" in c.lower() or "event" in c.lower() or "onset" in c.lower()}
    if extra_forbidden: raise ValueError(f"Evaluation-only fields must not enter rule engine: {sorted(extra_forbidden)}")
    results=[]
    for pid,g in measurements.sort_values(["patient_id","day_index_0_based"]).groupby("patient_id", sort=False):
        g=g.reset_index(drop=True)
        wbase_values=[]; hbase_values=[]; whistory=[]; hhistory=[]
        for _,r in g.iterrows():
            w=pd.to_numeric(pd.Series([r.weight_kg]),errors="coerce").iloc[0]
            h=pd.to_numeric(pd.Series([r.resting_hr_bpm]),errors="coerce").iloc[0]
            imp=pd.to_numeric(pd.Series([r.foot_impedance_ohm]),errors="coerce").iloc[0]
            wvalid=(r.weight_signal_quality=="valid" and np.isfinite(w) and 20 <= w <= 250)
            hvalid=(r.heart_rate_signal_quality=="valid" and np.isfinite(h) and 40 <= h <= 180)
            ivalid=(r.impedance_signal_quality=="valid" and np.isfinite(imp) and 100 <= imp <= 2000)
            wready=len(wbase_values)>=BASELINE_N
            hready=len(hbase_values)>=BASELINE_N
            wb=float(np.median(wbase_values)) if wready else np.nan
            hb=float(np.median(hbase_values)) if hready else np.nan
            w_over=bool(wvalid and wready and w >= wb*(1+WEIGHT_RISE_PCT/100))
            h_over=bool(hvalid and hready and h >= hb*(1+HR_RISE_PCT/100))
            if wvalid and wready: whistory=(whistory+[w_over])[-WINDOW:]
            if hvalid and hready: hhistory=(hhistory+[h_over])[-WINDOW:]
            wp=len(whistory)==WINDOW and sum(whistory)>=HITS
            hp=len(hhistory)==WINDOW and sum(hhistory)>=HITS
            # Use valid observations from the start-up period to establish personal baselines.
            if wvalid and not wready: wbase_values.append(float(w))
            if hvalid and not hready: hbase_values.append(float(h))
            baseline_ready=(len(wbase_values)>=BASELINE_N and len(hbase_values)>=BASELINE_N)
            # A warning can be issued from a verified channel even if another channel is unavailable.
            if wp and hp:
                state="ELEVATED_PATTERN"; reason="Both weight and resting-HR thresholds persisted in >=2 of the last 3 valid evaluated sessions."
            elif wp:
                state="WATCH"; reason="Weight threshold persisted in >=2 of the last 3 valid evaluated sessions."
            elif hp:
                state="WATCH"; reason="Resting-HR threshold persisted in >=2 of the last 3 valid evaluated sessions."
            elif not baseline_ready:
                state="CALIBRATING"; reason=f"Personal baseline not ready: needs {BASELINE_N} valid weight and {BASELINE_N} valid HR observations."
            elif not wvalid and not hvalid:
                state="INSUFFICIENT_DATA"; reason="Neither weight nor heart-rate channel is valid today."
            elif not IMPEDANCE_VERIFIED or not ivalid:
                state="INSUFFICIENT_DATA"; reason="No weight/HR rule triggered, but impedance is unverified or unavailable; cannot call this medically normal."
            else:
                state="NO_RULE_TRIGGER"; reason="No configured weight/HR rule triggered in valid data; this is not a medical all-clear."
            results.append({
                "patient_id":str(pid),"day_index_0_based":int(r.day_index_0_based),"date":str(r.date),
                "weight_kg":float(w) if np.isfinite(w) else np.nan,
                "resting_hr_bpm":float(h) if np.isfinite(h) else np.nan,
                "foot_impedance_ohm":float(imp) if np.isfinite(imp) else np.nan,
                "weight_quality":str(r.weight_signal_quality),"heart_rate_quality":str(r.heart_rate_signal_quality),"impedance_quality":str(r.impedance_signal_quality),
                "weight_valid":bool(wvalid),"heart_rate_valid":bool(hvalid),"impedance_valid_proxy":bool(ivalid),
                "weight_baseline_kg":wb,"heart_rate_baseline_bpm":hb,
                "weight_rise_pct_from_baseline":(100*(w/wb-1)) if wvalid and wready and wb>0 else np.nan,
                "heart_rate_rise_pct_from_baseline":(100*(h/hb-1)) if hvalid and hready and hb>0 else np.nan,
                "weight_threshold_exceeded":w_over,"heart_rate_threshold_exceeded":h_over,
                "weight_persistent":bool(wp),"heart_rate_persistent":bool(hp),
                "alert_state":state,"reason":reason,
                "rule_alert":bool(state in ("WATCH","ELEVATED_PATTERN")),
                "evaluable_for_weight_hr_rule_metrics":bool(baseline_ready and wvalid and hvalid)
            })
    return pd.DataFrame(results)

def calc_metrics(pred, labels, event_labels):
    df=pred.merge(labels,on=["patient_id","day_index_0_based"],how="left",validate="one_to_one")
    # After initial calibration days only; ground truth is used here, after predictions.
    scored=df[df.day_index_0_based>=BASELINE_N].copy()
    alert=scored.rule_alert
    normal=scored.worsening_period_label_EVALUATION_ONLY.eq(0)
    worsening=scored.worsening_period_label_EVALUATION_ONLY.eq(1)
    evaluable_normal=normal & scored.evaluable_for_weight_hr_rule_metrics
    specificity=float((~alert[evaluable_normal]).mean()) if evaluable_normal.any() else None
    sensitivity=float(alert[worsening].mean()) if worsening.any() else None
    # Overall accuracy on all valid/evaluable labelled days is defined as binary alert vs worsening.
    evaluable=scored.evaluable_for_weight_hr_rule_metrics & scored.worsening_period_label_EVALUATION_ONLY.notna()
    accuracy=float((alert[evaluable]==scored.loc[evaluable,"worsening_period_label_EVALUATION_ONLY"].eq(1)).mean()) if evaluable.any() else None
    # Worsening intervals caught at least once, using event labels only for evaluation.
    evrows=[]
    for _,lab in event_labels.dropna(subset=["simulated_onset_day","simulated_event_day"]).iterrows():
        sub=df[df.patient_id.astype(str)==str(lab.patient_id)].sort_values("day_index_0_based")
        target=sub[sub.rule_alert & (sub.day_index_0_based>=int(lab.simulated_onset_day)) & (sub.day_index_0_based<int(lab.simulated_event_day))]
        first=int(target.day_index_0_based.iloc[0]) if len(target) else np.nan
        evrows.append({"patient_id":lab.patient_id,"simulated_onset_day":int(lab.simulated_onset_day),"simulated_event_day":int(lab.simulated_event_day),"event_caught":bool(len(target)),"first_warning_day":first,"lead_days":int(lab.simulated_event_day)-first if len(target) else np.nan})
    ev=pd.DataFrame(evrows)
    caught=ev[ev.event_caught] if len(ev) else ev
    # Count false alert episode starts on labelled normal days, not individual alert-days.
    scored["prior_alert"] = scored.groupby("patient_id").rule_alert.shift(1,fill_value=False)
    scored["episode_start"] = scored.rule_alert & ~scored.prior_alert
    false_episodes=int((scored.episode_start & scored.worsening_period_label_EVALUATION_ONLY.eq(0)).sum())
    patient_months=len(scored)/30.4375
    metrics={
        "patients_in_split":int(df.patient_id.nunique()),"daily_records":int(len(df)),
        "event_cases":int(len(ev)),"events_caught_during_synthetic_worsening":int(ev.event_caught.sum()) if len(ev) else 0,
        "event_sensitivity_patient_level":float(ev.event_caught.mean()) if len(ev) else None,
        "median_lead_days_among_caught_events":float(caught.lead_days.median()) if len(caught) else None,
        "worsening_day_sensitivity_all_post_calibration_days":sensitivity,
        "specificity_on_weight_hr_evaluable_synthetic_normal_days":specificity,
        "evaluable_normal_days":int(evaluable_normal.sum()),
        "false_alert_days_on_evaluable_normal_days":int((alert & evaluable_normal).sum()),
        "overall_accuracy_on_evaluable_days_only":accuracy,
        "evaluable_days_for_accuracy":int(evaluable.sum()),
        "weight_hr_evaluable_coverage_post_calibration":float(scored.evaluable_for_weight_hr_rule_metrics.mean()) if len(scored) else None,
        "false_alert_episode_starts_on_synthetic_normal_days":false_episodes,
        "false_alert_episodes_per_patient_month_all_post_calibration_days":float(false_episodes/patient_months) if patient_months else None,
        "alert_state_counts":{str(k):int(v) for k,v in df.alert_state.value_counts().items()},
        "warning_rate_on_synthetic_worsening_days":float(alert[worsening].mean()) if worsening.any() else None,
        "interpretation":"Synthetic-only estimates; not clinical accuracy. A no-alert output is not a declaration of medical normality."
    }
    return metrics,df,ev

def main():
    OUT.mkdir(exist_ok=True,parents=True)
    x=pd.read_csv(DATA/"measurements_only.csv")
    labels=pd.read_csv(DATA/"daily_evaluation_labels_ONLY.csv")
    events=pd.read_csv(DATA/"event_labels_ONLY.csv")
    # Enforce strict separation of measurements from labels before inference.
    split=make_patient_split(events)
    split.to_csv(OUT/"patient_level_split.csv",index=False)
    test_ids=set(split.loc[split.split=="held_out_test","patient_id"].astype(str))
    dev_ids=set(split.loc[split.split=="development","patient_id"].astype(str))
    # Rules are run independently on patient data. No label is passed to run_rules().
    dev_pred=run_rules(x[x.patient_id.astype(str).isin(dev_ids)].copy())
    test_pred=run_rules(x[x.patient_id.astype(str).isin(test_ids)].copy())
    dev_metrics,dev_merged,dev_events=calc_metrics(dev_pred,labels[labels.patient_id.astype(str).isin(dev_ids)],events[events.patient_id.astype(str).isin(dev_ids)])
    test_metrics,test_merged,test_events=calc_metrics(test_pred,labels[labels.patient_id.astype(str).isin(test_ids)],events[events.patient_id.astype(str).isin(test_ids)])
    # Full held-out predictions are output only after rules have produced predictions.
    test_merged.to_csv(OUT/"held_out_daily_predictions_and_evaluation_labels.csv",index=False)
    test_events.to_csv(OUT/"held_out_event_detection_results.csv",index=False)
    dev_merged.to_csv(OUT/"development_daily_predictions_and_evaluation_labels.csv",index=False)
    dev_events.to_csv(OUT/"development_event_detection_results.csv",index=False)
    # Summary confusion matrix on held-out evaluable days only.
    scored=test_merged[test_merged.day_index_0_based>=BASELINE_N].copy()
    ev=scored[scored.evaluable_for_weight_hr_rule_metrics & scored.worsening_period_label_EVALUATION_ONLY.notna()].copy()
    y=ev.worsening_period_label_EVALUATION_ONLY.eq(1); p=ev.rule_alert
    cm={"TP":int((p&y).sum()),"FP":int((p&~y).sum()),"TN":int((~p&~y).sum()),"FN":int((~p&y).sum())}
    summary={
        "dataset_type":"synthetic only",
        "input_file":"data/measurements_only.csv",
        "input_columns_used":sorted(REQUIRED),
        "label_leakage_control":"Rule engine accepts only measurements file; labels are merged after predictions for evaluation.",
        "split":{"seed":SEED,"method":"70/30 stratified by event-assignment group; patient-level split, all days for a patient remain in one split","development_patients":len(dev_ids),"held_out_test_patients":len(test_ids)},
        "rule_parameters":{"baseline":"median of first 7 valid observations separately for weight and resting HR","weight_trigger":f"weight >= personal baseline +{WEIGHT_RISE_PCT}%","resting_hr_trigger":f"resting HR >= personal baseline +{HR_RISE_PCT}%","persistence":"at least 2 threshold exceedances among the last 3 valid evaluated sessions for that signal","one_persistent_signal":"WATCH","both_persistent_signals":"ELEVATED_PATTERN","impedance":"Recorded and quality-checked, but not used for alerting because the hardware front-end is not verified","normal_state":"NO_RULE_TRIGGER means no configured rule fired; it is not a medical all-clear","missing_or_unverified":"INSUFFICIENT_DATA or CALIBRATING; not normal"},
        "development_metrics":dev_metrics,
        "held_out_test_metrics":test_metrics,
        "held_out_evaluable_day_confusion_matrix":cm,
        "limitations":["All measurements and labels are synthetic and hand-specified; metrics measure agreement with the simulator's labels, not clinical accuracy.","No validated clinical thresholds are implied by +2% weight or +15% resting heart rate.","Foot-to-foot impedance is not used in alert decisions until the physical measurement circuit and safety are verified.","No real patient, sensor, BCG, ECG or hospital outcome validation is included.","A no-alert output cannot rule out deterioration; clinical review and local medical guidance remain necessary."]
    }
    (OUT/"metrics_summary.json").write_text(json.dumps(summary,indent=2))
    # Compact performance figure. Accuracy is explicitly evaluable-only.
    vals=[100*(test_metrics[k] or 0) for k in ["event_sensitivity_patient_level","worsening_day_sensitivity_all_post_calibration_days","specificity_on_weight_hr_evaluable_synthetic_normal_days","overall_accuracy_on_evaluable_days_only"]]
    labs=["Event sensitivity\n(patient-level)","Worsening-day\nsensitivity","Specificity\n(evaluable normal days)","Accuracy\n(evaluable days only)"]
    fig,ax=plt.subplots(figsize=(10,5.4)); bars=ax.bar(labs,vals)
    ax.set_ylim(0,100); ax.set_ylabel("Percent (%)"); ax.set_title("SteadyStep rule replay — held-out synthetic patients")
    ax.grid(axis="y",alpha=.25)
    for b,v in zip(bars,vals): ax.text(b.get_x()+b.get_width()/2,b.get_height()+1,f"{v:.1f}%",ha="center",va="bottom")
    fig.text(.5,.01,"Synthetic labels only. Not clinical accuracy. Accuracy/specificity use evaluable days only; impedance is unverified.",ha="center",fontsize=9)
    fig.tight_layout(rect=[0,0.06,1,1]); fig.savefig(OUT/"held_out_performance.png",dpi=180); plt.close(fig)
    print(json.dumps({"development_patients":len(dev_ids),"held_out_test_patients":len(test_ids),"held_out_test_metrics":test_metrics,"confusion_matrix":cm,"output_dir":str(OUT)},indent=2))
if __name__=="__main__": main()

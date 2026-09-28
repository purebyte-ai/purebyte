"""Draws every chart of the README, in a light and a dark variant, from the published numbers.

    pip install matplotlib
    python docs/assets/make_charts.py            # writes docs/assets/*-{light,dark}.png

Every number below is copied from benchmarks/RESULTS.md, docs/performance.md or the source named next to it; change
them there first. Nothing here is measured or computed from data: the script only draws.
"""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FixedLocator, FuncFormatter, NullFormatter, NullLocator  # noqa: E402

OUT = os.path.dirname(os.path.abspath(__file__))

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Segoe UI", "Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
    "axes.titleweight": "bold",
    "svg.fonttype": "none",
})

THEMES = {
    "dark": dict(bg="#0D0F13", fg="#EDEEF0", dim="#A9AFB8", faint="#6E7682", grid="#232833", ours="#C6F16B",
                 ours_edge="#C6F16B", other="#4A5466", other_edge="#4A5466", accent="#FF8A65", teal="#5EEAD4"),
    "light": dict(bg="#FFFFFF", fg="#0D0F13", dim="#4A515C", faint="#8A919B", grid="#E6E9ED", ours="#8BC34A",
                  ours_edge="#33691E", other="#B4BBC5", other_edge="#B4BBC5", accent="#E4572E", teal="#0F9D8A"),
}

# ---------------------------------------------------------------------------------------------------------------------
# The numbers
# ---------------------------------------------------------------------------------------------------------------------

# PII Masking Benchmark, mean F2 of its six tasks (benchmarks/RESULTS.md). Other models: the public leaderboard,
# piimb/pii-masking-benchmark-results, retrieved 2026-09-23: (name, total parameters, mean F2).
PII_F2, PII_PARAMS = 0.7689, 8.3e6
PIIMB_LEADERBOARD = [
    ("OpenMed/privacy-filter-multilingual-v2", 1399604809, 0.8359),
    ("OpenMed/OpenMed-PII-SuperClinical-Large-434M-v1", 434120810, 0.8349),
    ("knowledgator/gliner-stream-pii-v1.0", 676575232, 0.8277),
    ("OpenMed/OpenMed-PII-SuperClinical-Small-44M-v1", 141385834, 0.8163),
    ("OpenMed/privacy-filter-nemotron-v2", 1399607373, 0.8093),
    ("fastino/gliner2-privacy-filter-PII-multi", 307098645, 0.7997),
    ("nvidia/gliner-PII", 445463040, 0.7992),
    ("antoniogr7/pii-identifier-v0.1", 149690223, 0.7957),
    ("hivetrace/gliner-guard-omni", 307098645, 0.7879),
    ("fastino/gliner2-multi-v1", 307098645, 0.7847),
    ("knowledgator/gliner-pii-large-v1.0", 440220163, 0.7795),
    ("kalyan-ks/ettin-68m-nemotron-pii", 68462187, 0.7775),
    ("kalyan-ks/ettin-32m-nemotron-pii", 32072171, 0.7548),
    ("fastino/gliner2-large-v1", 486444053, 0.7455),
    ("gravitee-io/bert-small-pii-detection", 28527155, 0.7332),
    ("OpenMed/privacy-filter-nemotron", 1399607373, 0.7051),
    ("OpenMed/privacy-filter-multilingual", 1399604809, 0.7008),
    ("kalyan-ks/ettin-17m-nemotron-pii", 16890731, 0.6895),
    ("bardsai/eu-pii-anonimization-multilang", 277506117, 0.6847),
    ("scanpatch/pii-ner-nemotron", 558897207, 0.6797),
    ("knowledgator/gliner-pii-base-v1.0", 166023936, 0.6784),
    ("urchade/gliner_multi_pii-v1", 288949504, 0.6670),
    ("openai/privacy-filter", 1399486865, 0.6621),
    ("fastino/gliner2-base-v1", 208476821, 0.6490),
    ("tabularisai/eu-pii-safeguard", 558916682, 0.6306),
    ("gretelai/gretel-gliner-bi-small-v1.0", 189107328, 0.6285),
    ("lakshyakh93/deberta_finetuned_pii", 138690932, 0.6255),
    ("gretelai/gretel-gliner-bi-base-v1.0", 242281344, 0.6028),
    ("LiquidAI/LFM2.5-Encoder-350M-PII-Detector", 354648993, 0.5969),
    ("presidio/en_core_web_lg", 106175470, 0.5858),
    ("nationaldesignstudio/rampart", 18434723, 0.5748),
    ("HikmaAI/hikmaai-distilbert-pii", 134760995, 0.5196),
    ("iiiorg/piiranha-v1-detect-personal-information", 278232594, 0.5163),
    ("ai4privacy/llama-ai4privacy-multilingual-categorical-anonymiser-openpii", 149635624, 0.4582),
    ("ai4privacy/llama-ai4privacy-multilingual-anonymiser-openpii", 149607171, 0.3838),
]
PIIMB_TASKS = ["ai4privacy-en", "ai4privacy-multi", "nemotron-pii", "gretel", "privy", "mapa-eur-lex"]
PIIMB_BY_TASK = [  # (name, per-task F2), benchmarks/RESULTS.md
    ("PureByte pii 1.0.0 · 8.3 M parameters", [0.9569, 0.9498, 0.8918, 0.9463, 0.5360, 0.3325]),
    ("openai/privacy-filter · 1.4 B", [0.8426, 0.8052, 0.6068, 0.8687, 0.5362, 0.3129]),
    ("Leaderboard #1 · 1.4 B", [0.9754, 0.9701, 0.9070, 0.9727, 0.8176, 0.3728]),
]

# CredData TEST, 164 repositories, 4,219 credential lines (benchmarks/RESULTS.md): (name, F1, CI low, CI high).
CREDDATA = [
    ("PureByte secrets-code · 1 MB", 0.797, 0.719, 0.860),
    ("PureByte, ensemble of three", 0.805, 0.731, 0.863),
    ("gitleaks 8.30.1", 0.337, 0.263, 0.421),
    ("trufflehog 3.97.6 (no verification)", 0.047, 0.017, 0.092),
]
CREDDATA_FAMILIES = [  # (family, positive units, recall PureByte, gitleaks, trufflehog)
    ("Passwords", 1125, 0.88, 0.06, 0.00),
    ("Tokens (Bearer, JWT, auth)", 795, 0.69, 0.44, 0.00),
    ("URL credentials, Basic auth", 650, 0.99, 0.01, 0.08),
    ("Provider formats (AWS, Google, Slack...)", 393, 0.85, 0.28, 0.05),
    ("Generic keys and secrets", 382, 0.82, 0.49, 0.00),
    ("Private keys (PEM...)", 182, 0.90, 0.84, 0.14),
    ("UUIDs, salts, nonces ¹", 692, 0.12, 0.00, 0.00),
]

# secrets-bin recall probe (benchmarks/RESULTS.md): (metric, secrets-bin, strings + regular expressions).
BINARY_PROBE = [
    ("Recall\n360 planted credentials", 1.000, 0.803),
    ("Precision\n400 look-alike traps", 0.989, 0.876),
]

# pii, values left unmasked on the contexts test set, mean of three training runs (benchmarks/RESULTS.md).
PII_CONTEXTS = [("Code", 38, 1.3), ("Structured\nrecords", 28, 1.9), ("Logs", 21, 0.04), ("E-mails", 21, 19),
                ("Prose", 27, 27)]

# Latency of one window, median of 40 runs, idle AMD Ryzen 9 5900X (docs/performance.md): ms by thread count.
THREADS = [1, 2, 4, 8, 12]
WINDOW_MS = {
    "secrets-code": {64: [2.966, 1.617, 0.910, 0.817, 0.923], 256: [12.005, 6.391, 3.538, 2.807, 3.127],
                     1024: [50.431, 26.507, 14.486, 10.504, 11.413]},
    "pii": {64: [3.200, 1.866, 1.044, 0.886, 1.043], 256: [13.069, 7.002, 4.125, 3.096, 3.408],
            1024: [54.770, 28.714, 19.089, 13.677, 13.485]},
    "secrets-bin": {64: [5.917, 3.158, 1.737, 1.449, 1.709], 256: [24.265, 12.665, 6.799, 4.951, 5.689],
                    1024: [102.463, 53.250, 30.449, 24.472, 22.787]},
}

# One decision on a short input (docs/performance.md#compared-with-laya-and-jev): (label, ms low, ms high, kind).
# Laya on this CPU: the laya 0.3.6 package, English checkpoint, PyTorch 2.14 (CPU), a 3-option question about one line
# of code (173 bytes, 190 tokens with the question), median of 15, same idle machine as PureByte.
DECISION_MS = [
    ("PureByte secrets-code · 64 bytes · 8 CPU threads (a shorter input than Laya's)", 0.82, 0.82, "ours"),
    ("PureByte secrets-code · 256 bytes · 8 CPU threads", 2.81, 2.81, "ours"),
    ("PureByte secrets-code · 1,024 bytes · 8 CPU threads", 10.5, 10.5, "ours"),
    ("PureByte secrets-code · 256 bytes · 1 CPU thread", 12.0, 12.0, "ours"),
    ("Laya · NVIDIA T4 GPU · vendor figure", 32.8, 39.5, "gpu"),
    ("TypeSafe Jev · hosted API · third-party p50", 236, 276, "api"),
    ("Laya · same CPU · 12 threads", 373, 373, "cpu"),
    ("Laya · same CPU · 1 thread", 1382, 1382, "cpu"),
]

# Training time of the released recipes on one AMD Radeon RX 6800 XT, every seed and control (docs/performance.md,
# Training time); Laya: its fine-tuning notebook on two T4 GPUs, "roughly 4-5 hours for 4 epochs over ~30k questions"
# (github.com/NandhaKishorM/laya), on top of a pretrained encoder.
TRAIN_MIN = [
    ("secrets-code · 3 seeds + control", 24.5, "ours"),
    ("secrets-bin · 6 seeds + control", 58, "ours"),
    ("pii · 6 seeds + 2 controls", 60, "ours"),
    ("Laya · fine-tuning only · 2× T4", 240, "other", 240, 300),
]

# The three example specialists, each against a baseline (benchmarks/RESULTS.md): openai/privacy-filter on PIIMB,
# gitleaks on CredData, and our own strings + regular expressions rival on our own synthetic binary probe, whose
# credentials come from the training generator (no public benchmark exists).
EXAMPLES = [
    ("pii · PII Masking Benchmark F2", 0.769, "ours"),
    ("openai/privacy-filter (1.4 B)", 0.662, "other"),
    ("secrets-code · CredData F1", 0.797, "ours"),
    ("gitleaks 8.30.1", 0.337, "other"),
    ("secrets-bin · recall, synthetic probe", 1.000, "ours"),
    ("strings + regex (our baseline)", 0.803, "other"),
]

# Parameters (model cards; Laya: its model card; openai/privacy-filter: the PIIMB leaderboard's total, and its model
# card's active count).
PARAMS = [
    ("PureByte · secrets-code", 1.93e6, "ours"),
    ("PureByte · pii", 8.3e6, "ours"),
    ("PureByte · secrets-bin", 54.2e6, "ours"),
    ("Laya (ModernBERT-large + head)", 421e6, "other"),
    ("openai/privacy-filter (about 50 M active)", 1.3995e9, "other"),
]

# ---------------------------------------------------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------------------------------------------------


def figure(t, w, h):
    fig = plt.figure(figsize=(w, h), dpi=150)
    fig.patch.set_facecolor(t["bg"])
    return fig


def clean(ax, t, grid="x"):
    ax.set_facecolor(t["bg"])
    for s in ax.spines.values():
        s.set_visible(False)
    ax.tick_params(colors=t["fg"], labelsize=10.5, length=0)
    if grid:
        ax.grid(axis=grid, color=t["grid"], linewidth=1)
    ax.set_axisbelow(True)


def title(ax, t, text, sub=None, size=14):
    """Title of one panel of a figure with several panels."""
    ax.set_title(text, color=t["fg"], fontsize=size, loc="left", pad=26 if sub else 12)
    if sub:
        ax.text(0, 1.02, sub, transform=ax.transAxes, color=t["dim"], fontsize=10, va="bottom")


def frame(fig, ax, t, head, sub=None, note=None, legend_row=False, xlabel_lines=1):
    """Lays out a one-panel figure: title, subtitle and footnote aligned with the left edge of the tick labels."""
    fig.canvas.draw()
    r = fig.canvas.get_renderer()
    w, h = fig.get_figwidth(), fig.get_figheight()
    label_w = max([lb.get_window_extent(r).width for lb in ax.get_yticklabels() if lb.get_text()] or [0]) / fig.dpi
    margin = 0.08
    left = (margin + label_w + 0.12) / w
    top_in = 0.42 + (0.3 if sub else 0) + (0.36 if legend_row else 0)
    note_lines = len(note.splitlines()) if note else 0
    bottom_in = 0.2 + 0.26 * xlabel_lines + 0.17 * note_lines + (0.12 if note else 0)
    fig.subplots_adjust(left=left, right=1 - 0.35 / w, top=1 - top_in / h, bottom=bottom_in / h)
    x = margin / w
    fig.text(x, 1 - 0.1 / h, head, color=t["fg"], fontsize=14.5, fontweight="bold", va="top")
    if sub:
        fig.text(x, 1 - 0.42 / h, sub, color=t["dim"], fontsize=10.2, va="top")
    if note:
        fig.text(x, 0.06 / h, note, color=t["faint"], fontsize=8.4, va="bottom", linespacing=1.4)
    return x


def footnote(fig, t, text, y=0.012):
    fig.text(0.012, y, text, color=t["faint"], fontsize=8.3, va="bottom")


def legend(fig, ax, t, handles_labels=None, y_in=None, ncol=3):
    """A row of legend entries under the subtitle, aligned with the title."""
    h, l = handles_labels or ax.get_legend_handles_labels()
    w, hh = fig.get_figwidth(), fig.get_figheight()
    leg = fig.legend(h, l, loc="upper left", bbox_to_anchor=(0.08 / w - 0.008, 1 - (y_in or 0.7) / hh), ncol=ncol,
                     frameon=False, fontsize=9.8, handlelength=1.4, columnspacing=1.6)
    for txt in leg.get_texts():
        txt.set_color(t["fg"])


def save(fig, name, theme, pad=0.2):
    fig.savefig(os.path.join(OUT, f"{name}-{theme}.png"), facecolor=fig.get_facecolor(), bbox_inches="tight",
                pad_inches=pad)
    plt.close(fig)


def hbars(ax, t, rows, xmax, fmt, log=False, label_size=10.5, bar_h=0.62):
    """rows: (label, value, kind[, low, high]); drawn top to bottom. On a log axis the values are dots, not bars: a bar
    would start at an arbitrary left edge, and its length would show neither the value nor the ratios."""
    n = len(rows)
    for i, r in enumerate(rows):
        label, v, kind = r[0], r[1], r[2]
        y = n - 1 - i
        ours = kind == "ours"
        color = t["ours"] if ours else (t["accent"] if kind == "accent" else t["other"])
        edge = t["ours_edge"] if ours else color
        if log:
            ax.axhline(y, color=t["grid"] if "grid" in t else t["dim"], linewidth=0.5, alpha=0.35, zorder=1)
            ax.plot([v], [y], marker="o", markersize=11, markerfacecolor=color, markeredgecolor=edge,
                    markeredgewidth=1.4, linestyle="none", zorder=4)
        else:
            ax.barh(y, v, color=color, edgecolor=edge, linewidth=1.1, height=bar_h, zorder=2)
        end = v
        if len(r) > 3 and r[4] > r[3]:
            ax.plot([r[3], r[4]], [y, y], color=t["fg"], linewidth=1.3, zorder=3)
            ax.plot([r[3]] * 2, [y - .14, y + .14], color=t["fg"], linewidth=1.3, zorder=3)
            ax.plot([r[4]] * 2, [y - .14, y + .14], color=t["fg"], linewidth=1.3, zorder=3)
            end = r[4]
        x = end * 1.45 if log else end + xmax * 0.012
        ax.text(x, y, fmt(r), va="center", color=t["fg"], fontsize=label_size,
                fontweight="bold" if ours else "normal", zorder=4)
    ax.set_yticks(range(n), [r[0] for r in rows][::-1])
    for lbl, r in zip(ax.get_yticklabels(), rows[::-1]):
        lbl.set_color(t["fg"] if r[2] == "ours" else t["dim"])
        lbl.set_fontweight("bold" if r[2] == "ours" else "normal")
    ax.set_ylim(-0.6, n - 0.4)


def ms_label(v):
    if v < 1:
        return f"{v:.2f} ms"
    if v < 100:
        return f"{v:.1f}".removesuffix(".0") + " ms"
    if v < 1000:
        return f"{v:.0f} ms"
    return f"{v / 1000:.1f} s"


def log_ms_axis(ax, t, lo, hi):
    ax.set_xscale("log")
    ax.set_xlim(lo, hi)
    ticks = [x for x in (0.1, 1, 10, 100, 1000, 10000) if lo <= x <= hi]
    ax.set_xticks(ticks)
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{x:g} ms" if x < 1000 else f"{x / 1000:g} s"))
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)


def params_label(v):
    return f"{v / 1e9:.1f} B" if v >= 1e9 else (f"{v / 1e6:.0f} M" if v >= 1e7 else f"{v / 1e6:.1f} M")


# --- The hero: four panels --------------------------------------------------------------------------------------------

def hero(theme):
    """The architecture in four numbers: time per decision on a CPU, size, training cost, and three examples."""
    t = THEMES[theme]
    fig = figure(t, 13.2, 7.9)
    gs = fig.add_gridspec(2, 2, hspace=0.62, wspace=0.66, left=0.2, right=0.95, top=0.9, bottom=0.1)

    ax = fig.add_subplot(gs[0, 0])
    clean(ax, t)
    rows = [("PureByte · desktop CPU", 2.81, "ours"),
            ("Laya · T4 GPU", 32.8, "other", 32.8, 39.5),
            ("TypeSafe Jev · hosted API", 236, "other", 236, 276),
            ("Laya · same desktop CPU", 373, "other")]
    hbars(ax, t, rows, 1, lambda r: ms_label(r[1]) if len(r) == 3 else f"{r[3]:.0f}-{r[4]:.0f} ms", log=True)
    log_ms_axis(ax, t, 0.5, 20000)
    title(ax, t, "Time for one decision", "A short input: one line of code · log scale · lower is better")

    ax = fig.add_subplot(gs[0, 1])
    clean(ax, t)
    hbars(ax, t, PARAMS, 1, lambda r: params_label(r[1]), log=True)
    ax.set_xscale("log")
    ax.set_xlim(1e6, 2e10)
    ax.set_xticks([1e6, 1e7, 1e8, 1e9, 1e10], ["1 M", "10 M", "100 M", "1 B", "10 B"])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    title(ax, t, "Model size", "Parameters · log scale · the three PureByte examples in green")

    ax = fig.add_subplot(gs[1, 0])
    clean(ax, t)
    hbars(ax, t, TRAIN_MIN, 330, lambda r: f"{r[1]:g} min" if len(r) == 3 else f"{r[3] / 60:g}-{r[4] / 60:g} h")
    ax.set_xlim(0, 330)
    ax.set_xticks([0, 60, 120, 180, 240, 300], ["0", "1 h", "2 h", "3 h", "4 h", "5 h"])
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    title(ax, t, "Training a model", "From scratch on one consumer GPU · Laya fine-tunes a general model · lower is better")

    ax = fig.add_subplot(gs[1, 1])
    clean(ax, t)
    hbars(ax, t, EXAMPLES, 1, lambda r: f"{r[1]:.3f}", bar_h=0.7)
    ax.set_xlim(0, 1.12)
    ax.set_xticks([0, 0.2, 0.4, 0.6, 0.8, 1.0])
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    title(ax, t, "Three example specialists", "Against baselines; secrets-bin on a synthetic probe · higher is better")

    footnote(fig, t, "PureByte: our measurements of the released 1.0.0 files on an AMD Ryzen 9 5900X and an AMD Radeon "
             "RX 6800 XT (benchmarks/RESULTS.md, docs/performance.md); its decision is one 256-byte window run by the "
             "engine with 8 threads.\nLaya on the CPU: measured by us on the same machine. Laya on a T4 and Jev: "
             "Laya's model card; Laya's fine-tuning time: its README. Laya and Jev are general models that answer "
             "questions written at request\ntime; a PureByte model answers the one it was trained for. The "
             "secrets-bin probe is synthetic (its credentials come from the training generator) and there is no public "
             "benchmark of credentials in binaries yet.\nCredData ships its credentials with random values; with the "
             "original values restored, secrets-code scores F1 0.694 and gitleaks 0.329.", y=0.0)
    save(fig, "hero", theme)


# --- Speed ------------------------------------------------------------------------------------------------------------

def decision_speed(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 5.6)
    ax = fig.add_subplot(111)
    clean(ax, t)
    rows = []
    for label, lo, hi, kind in DECISION_MS:
        k = "ours" if kind == "ours" else "other"
        rows.append((label, lo, k, lo, hi) if hi > lo else (label, lo, k))
    hbars(ax, t, rows, 1, lambda r: ms_label(r[1]) if len(r) == 3 else f"{r[3]:.0f}-{r[4]:.0f} ms", log=True)
    log_ms_axis(ax, t, 0.4, 4000)
    frame(fig, ax, t, "One small decision: PureByte against Laya and TypeSafe Jev",
          "Median latency, log scale. PureByte and Laya on the same idle desktop CPU (AMD Ryzen 9 5900X); "
          "the GPU and API figures as published.",
          "Laya: laya 0.3.6, English checkpoint (421 M parameters), PyTorch 2.14 on the CPU, one 3-option question "
          "about one line of code (173 bytes; 190 tokens with the question), median of 15. T4: Laya's model card.\n"
          "Jev: third-party p50 cited by Laya. PureByte: the released secrets-code model in the purebyte engine, one "
          "window, median of 40. Laya and Jev answer questions written at request time;\na PureByte specialist answers "
          "the one it was trained for (docs/performance.md).")
    save(fig, "speed", theme)


def threads(theme):
    t = THEMES[theme]
    fig = figure(t, 12.6, 4.6)
    colors = {"secrets-code": t["ours"], "pii": t["teal"], "secrets-bin": t["accent"]}
    ticks = {64: [0.5, 1, 2, 5], 256: [2, 5, 10, 20], 1024: [10, 20, 50, 100]}
    for k, size in enumerate((64, 256, 1024)):
        ax = fig.add_subplot(1, 3, k + 1)
        clean(ax, t, grid="both")
        for name, by_size in WINDOW_MS.items():
            ys = by_size[size]
            ax.plot(THREADS, ys, marker="o", markersize=5, linewidth=2.2, color=colors[name], label=name)
            nudge = {"secrets-code": -4, "pii": 4}.get(name, 0)
            ax.annotate(f"{ys[-1]:.1f}" if ys[-1] >= 10 else f"{ys[-1]:.2f}", (12, ys[-1]), xytext=(7, nudge),
                        textcoords="offset points", color=colors[name], fontsize=9, va="center")
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(FixedLocator(ticks[size]))
        ax.yaxis.set_minor_locator(NullLocator())
        ax.yaxis.set_minor_formatter(NullFormatter())
        ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g} ms"))
        ax.set_ylim(ticks[size][0] * 0.8, ticks[size][-1] * 1.25)
        ax.set_xticks(THREADS)
        ax.set_xlim(0.5, 14.2)
        ax.tick_params(colors=t["dim"], labelsize=9.5)
        ax.set_xlabel("threads working on the window", color=t["dim"], fontsize=9.5)
        ax.set_title(f"{size if size < 1024 else '1,024'} bytes", color=t["fg"], fontsize=12, loc="left", pad=8)
    handles, labels = fig.axes[0].get_legend_handles_labels()
    w, h = fig.get_figwidth(), fig.get_figheight()
    fig.subplots_adjust(left=0.07, right=0.97, top=1 - 1.45 / h, bottom=1.05 / h, wspace=0.28)
    fig.text(0.08 / w, 1 - 0.1 / h, "One window, one to twelve threads", color=t["fg"], fontsize=14.5,
             fontweight="bold", va="top")
    fig.text(0.08 / w, 1 - 0.42 / h, "Median latency of one window of each released specialist, log scale. Every "
             "thread count gives bit-identical results.", color=t["dim"], fontsize=10.2, va="top")
    legend(fig, None, t, (handles, labels), y_in=0.72)
    fig.text(0.08 / w, 0.06 / h, "Median of 40 runs, idle AMD Ryzen 9 5900X (12 cores), release build, AVX2 kernel. "
             "Past 8 threads one window gets no faster, so the engine gives each window at most 8 and uses the rest "
             "for other windows\n(docs/performance.md).", color=t["faint"], fontsize=8.4, va="bottom", linespacing=1.4)
    save(fig, "speed-threads", theme)


# --- PII --------------------------------------------------------------------------------------------------------------

def piimb(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 6.6)
    ax = fig.add_subplot(111)
    clean(ax, t, grid="both")
    ax.set_xscale("log")
    top = max(r[2] for r in PIIMB_LEADERBOARD)
    for name, params, f2 in PIIMB_LEADERBOARD:
        special = name in ("openai/privacy-filter", "presidio/en_core_web_lg") or f2 == top
        color = t["accent"] if name == "openai/privacy-filter" else (t["teal"] if f2 == top else t["other"])
        ax.scatter(params, f2, s=70 if special else 40, color=color, zorder=3, linewidth=0)
    # the best score reached at each size or smaller, PureByte included
    pts = sorted([(p, f) for _, p, f in PIIMB_LEADERBOARD] + [(PII_PARAMS, PII_F2)])
    xs, ys, best = [], [], 0
    for p, f in pts:
        if f > best:
            if xs:
                xs.append(p)
                ys.append(best)
            best = f
            xs.append(p)
            ys.append(best)
    xs.append(3e9)
    ys.append(best)
    ax.plot(xs, ys, color=t["faint"], linewidth=1, linestyle=(0, (4, 3)), zorder=2)
    ax.text(1.35e7, 0.7755, "best score at this size or smaller", color=t["faint"], fontsize=8.5)
    ax.scatter(PII_PARAMS, PII_F2, s=520, marker="*", color=t["ours"], edgecolor=t["ours_edge"], linewidth=1.2,
               zorder=5)
    ax.annotate("PureByte pii · 4.5 MiB · CPU\n8.3 M parameters · 0.769", (PII_PARAMS, PII_F2), xytext=(-6, 30),
                textcoords="offset points", color=t["fg"], fontsize=12, fontweight="bold")
    ax.annotate("openai/privacy-filter\n1.4 B parameters · 0.662", (1.3995e9, 0.6621), xytext=(-150, -58),
                textcoords="offset points", color=t["accent"], fontsize=10.5, fontweight="bold",
                arrowprops=dict(arrowstyle="-", color=t["accent"], linewidth=1))
    ax.annotate("Presidio + spaCy · 0.586", (106175470, 0.5858), xytext=(-40, -34), textcoords="offset points",
                color=t["dim"], fontsize=9.5, arrowprops=dict(arrowstyle="-", color=t["faint"], linewidth=1))
    ax.annotate("#1 · 1.4 B parameters · 0.836", (1399604809, 0.8359), xytext=(-205, 10), textcoords="offset points",
                color=t["teal"], fontsize=10)
    ax.set_xlim(4e6, 3e9)
    ax.set_ylim(0.36, 0.88)
    ax.set_xticks([1e7, 1e8, 1e9], ["10 M", "100 M", "1 B"])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.tick_params(colors=t["dim"], labelsize=10)
    ax.set_xlabel("Model parameters (log scale)", color=t["dim"], fontsize=10.5)
    ax.set_ylabel("Mean F2 of the six tasks", color=t["dim"], fontsize=10.5)
    frame(fig, ax, t, "PII Masking Benchmark: smaller than every model on the leaderboard, ahead of 23 of its 35",
          "Every model that scores higher has at least 8 times as many parameters (68 M to 1.4 B).",
          "Other models: the public PIIMB leaderboard (sentences subset, character-level label-agnostic F2), retrieved "
          "2026-09-23, with its counts of total parameters (the privacy-filter models\nare mixtures of experts, about "
          "50 M active). PureByte pii: our measurement of the released file with the same metric "
          "(benchmarks/RESULTS.md); 8.3 M parameters, 6.3 M of them in its n-gram tables.",
          xlabel_lines=2)
    save(fig, "bench-piimb", theme)


def piimb_tasks(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 5.4)
    ax = fig.add_subplot(111)
    clean(ax, t, grid="y")
    n = len(PIIMB_TASKS)
    width = 0.26
    colors = [t["ours"], t["accent"], t["teal"]]
    for k, (name, vals) in enumerate(PIIMB_BY_TASK):
        xs = [i + (k - 1) * width for i in range(n)]
        ax.bar(xs, vals, width=width * 0.92, color=colors[k], edgecolor=t["ours_edge"] if k == 0 else colors[k],
               linewidth=1 if k == 0 else 0, label=name, zorder=2)
        for x, v in zip(xs, vals):
            ax.text(x, v + 0.012, f"{v:.2f}", ha="center", color=t["fg"] if k == 0 else t["dim"], fontsize=8.6,
                    fontweight="bold" if k == 0 else "normal")
    ax.set_xticks(range(n), PIIMB_TASKS)
    ax.set_ylim(0, 1.05)
    ax.tick_params(colors=t["dim"], labelsize=10)
    for lbl in ax.get_xticklabels():
        lbl.set_color(t["fg"])
    frame(fig, ax, t, "PII Masking Benchmark, task by task (F2)",
          "Ahead of OpenAI's filter on five tasks, 0.0002 behind it on privy; behind the leader on protocol traces (privy) and "
          "EU legal texts (mapa-eur-lex).",
          "PureByte pii 1.0.0 at its default operating point; the other models from the public leaderboard "
          "(benchmarks/RESULTS.md).", legend_row=True)
    legend(fig, ax, t)
    save(fig, "bench-piimb-tasks", theme)


def pii_contexts(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 5.0)
    ax = fig.add_subplot(111)
    clean(ax, t, grid="y")
    width = 0.36
    for k, (label, idx, color, edge) in enumerate([("Trained on public documents only", 1, t["other"], t["other"]),
                                                  ("Released recipe: documents + generated records, logs and code",
                                                   2, t["ours"], t["ours_edge"])]):
        xs = [i + (k - 0.5) * width for i in range(len(PII_CONTEXTS))]
        vals = [r[idx] for r in PII_CONTEXTS]
        ax.bar(xs, vals, width=width * 0.94, color=color, edgecolor=edge, linewidth=1 if k else 0, label=label,
               zorder=2)
        for x, v in zip(xs, vals):
            ax.text(x, v + 0.8, f"{v:g} %", ha="center", color=t["fg"] if k else t["dim"], fontsize=9.5,
                    fontweight="bold" if k else "normal")
    ax.set_xticks(range(len(PII_CONTEXTS)), [r[0] for r in PII_CONTEXTS])
    ax.set_ylim(0, 42)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g} %"))
    ax.tick_params(colors=t["dim"], labelsize=10)
    for lbl in ax.get_xticklabels():
        lbl.set_color(t["fg"])
    frame(fig, ax, t, "Personal data left unmasked, by kind of input (lower is better)",
          "2,000 generated documents with held-out values, formats and code; mean of three training runs.",
          "The test set comes from the same generator family as the generated training data, so the gains are "
          "optimistic; the PIIMB figures are independent (benchmarks/RESULTS.md).", legend_row=True, xlabel_lines=2)
    legend(fig, ax, t, ncol=2)
    save(fig, "pii-contexts", theme)


# --- Secrets ----------------------------------------------------------------------------------------------------------

def creddata(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 4.3)
    ax = fig.add_subplot(111)
    clean(ax, t)
    rows = [(n, f1, "ours" if n.startswith("PureByte") else "other", lo, hi) for n, f1, lo, hi in CREDDATA]
    hbars(ax, t, rows, 1, lambda r: f"{r[1]:.3f}")
    ax.set_xlim(0, 1)
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    frame(fig, ax, t, "Leaked credentials found in source code (F1)",
          "Samsung CredData TEST: 164 repositories, 4,219 labeled credential lines; 95 % bootstrap interval over "
          "repositories.",
          "All tools run by us on the same files with the same scoring code (benchmarks/RESULTS.md); trufflehog "
          "without verification, so nothing is sent to any service.\nCredData replaces its credentials with random "
          "values; with the original values restored, secrets-code scores 0.694 and gitleaks 0.329 "
          "(benchmarks/RESULTS.md#on-the-original-values).")
    save(fig, "bench-creddata", theme)


def creddata_families(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 6.4)
    ax = fig.add_subplot(111)
    clean(ax, t)
    rows = CREDDATA_FAMILIES
    n = len(rows)
    h = 0.26
    tools = [("PureByte secrets-code", 2, t["ours"], t["ours_edge"]), ("gitleaks 8.30.1", 3, t["other"], t["other"]),
             ("trufflehog 3.97.6", 4, t["faint"], t["faint"])]
    for k, (name, idx, color, edge) in enumerate(tools):
        ys = [n - 1 - i - (k - 1) * h for i in range(n)]
        vals = [r[idx] for r in rows]
        ax.barh(ys, vals, height=h * 0.9, color=color, edgecolor=edge, linewidth=1 if k == 0 else 0, label=name,
                zorder=2)
        for y, v in zip(ys, vals):
            ax.text(v + 0.01, y, f"{v:.2f}", va="center", color=t["fg"] if k == 0 else t["dim"], fontsize=8.8,
                    fontweight="bold" if k == 0 else "normal")
    ax.set_yticks(range(n), [f"{r[0]}  ·  {r[1]:,}" for r in rows][::-1])
    for lbl in ax.get_yticklabels():
        lbl.set_color(t["fg"])
    ax.set_xlim(0, 1.06)
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    frame(fig, ax, t, "Recall by family of credential",
          "CredData TEST, 164 repositories; after each family, its count of labeled lines. Passwords are where "
          "pattern scanners are blind.",
          "¹ By design, secrets-code reports a UUID, salt or nonce only under an unequivocal credential name; CredData "
          "labels most of them as credentials (benchmarks/RESULTS.md).", legend_row=True)
    legend(fig, ax, t)
    save(fig, "bench-creddata-families", theme)


def binary_probe(theme):
    t = THEMES[theme]
    fig = figure(t, 12, 3.9)
    ax = fig.add_subplot(111)
    clean(ax, t)
    n = len(BINARY_PROBE)
    h = 0.34
    for k, (name, idx, color, edge) in enumerate([("PureByte secrets-bin", 1, t["ours"], t["ours_edge"]),
                                                  ("strings + regular expressions", 2, t["other"], t["other"])]):
        ys = [n - 1 - i - (k - 0.5) * h for i in range(n)]
        vals = [r[idx] for r in BINARY_PROBE]
        ax.barh(ys, vals, height=h * 0.9, color=color, edgecolor=edge, linewidth=1 if k == 0 else 0, label=name,
                zorder=2)
        for y, v in zip(ys, vals):
            ax.text(v + 0.01, y, f"{v:.3f}", va="center", color=t["fg"] if k == 0 else t["dim"], fontsize=10,
                    fontweight="bold" if k == 0 else "normal")
    ax.set_yticks(range(n), [r[0].replace("\n", " · ") for r in BINARY_PROBE][::-1])
    for lbl in ax.get_yticklabels():
        lbl.set_color(t["fg"])
    ax.set_xlim(0, 1.08)
    ax.tick_params(axis="x", colors=t["dim"], labelsize=9.5)
    frame(fig, ax, t, "Credentials planted in real binaries: a synthetic probe",
          "2,000 windows of real binaries; the credentials, their names, the traps and the layouts come from the "
          "training generator (in-distribution).",
          "Our own exam: no public benchmark exists yet, and the regex rival was not built for the generator's formats. "
          "Recall 360 of 360, 95 % interval 0.990-1.000; over six seeds 0.989-1.000.\nNeither tool flags any of the "
          "1,200 untouched windows. Conditions in benchmarks/RESULTS.md.", legend_row=True)
    legend(fig, ax, t, ncol=2)
    save(fig, "bench-secrets-bin", theme)


if __name__ == "__main__":
    for th in THEMES:
        hero(th)
        decision_speed(th)
        threads(th)
        piimb(th)
        piimb_tasks(th)
        pii_contexts(th)
        creddata(th)
        creddata_families(th)
        binary_probe(th)
    print("written to", OUT)

#!/usr/bin/env python3
"""
banc_regression.py - banc de non-regression de l'emulateur CD-i 205

Lance chaque jeu de CDI-205/Games sans fenetre et sans son (vitesse
maximale, copie fixe d'une NVRAM initialisee, clic sur PLAY dans le shell a
la frame 1500 puis clics propres au jeu, horloge figee), garde une image toutes les N frames et une empreinte du son decode,
puis compare a une reference.

  python tools/banc_regression.py --reference     enregistre la reference
  python tools/banc_regression.py                 compare a la reference
  python tools/banc_regression.py --jeux inca,hotel   seulement ces jeux
  python tools/banc_regression.py --liste         liste les jeux connus

Options : --exe (defaut src/cdi_emu/bin/cdi_emu.exe), -j (lancements en
parallele, defaut 2), --frames (derniere frame, defaut 12000), --pas
(une image toutes les N frames, defaut 2000).

Les references et les differences sont des images de jeux : elles restent
dans CDI-205/Regression (ignore par git). Un jeu absent du disque est
ignore. Les images d'une meme version sont identiques d'une passe a
l'autre (emulation deterministe) : toute difference vient du code.
"""

import argparse, hashlib, os, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = os.path.join(ROOT, "CDI-205", "Games")
OUT = os.path.join(ROOT, "CDI-205", "Regression")
PORTABLE = os.path.join(ROOT, "src", "cdi_emu", "dist", "cdi205-portable")

PLAY = "1500:578,345"                 # bouton PLAY CD-I du shell, a la frame 1500
# NVRAM de depart, copiee avant chaque jeu (une NVRAM vierge fait refuser
# certains disques par le shell, ex. Live Without Monty Python)
NVRAM = os.path.join(OUT, "nvram_base")

# nom court, image (relative a CDI-205/Games), cartouche FMV, clics suivants
# (EMU_CLICKS : frame:x,y[,boutons];... en unites du pointeur)
JEUX = [
    ("asterix",   "ASTERIX/Asterix - Le Defi de Cesar (France).cue", 0, ""),
    ("battleship","Battleship (USA Europe)/Battleship (USA, Europe).cue", 0, ""),
    ("cdshoot",   "CD_SHOOT/CD-SHOOT.cue", 0, ""),
    ("cybercity", "Escape from CyberCity (USA Europe).chd", 0, ""),
    ("facekitchen","Face Kitchen (Europe)/Face Kitchen (Europe).cue", 0, ""),
    ("harvest",   "HARVEST_OF_TNE_SUN/Harvest_of_the_sun.cue", 0, ""),
    ("hotelmario","HOTEL MARIO/Hotel Mario (France).cue", 0, "13000:400,232"),
    ("inca",      "INCA/INCA.cue", 0, "5000:300,200"),
    ("tf1_1",     "JOURNAL_INTERACTIF_94_Disque1/TF1_1994_La_Retrospective.cue", 1, ""),
    ("tf1_2",     "JOURNAL_INTERACTIF_94_Disque2/TF1_1994_Le_Jeu.cue", 1, ""),
    ("joyofsex",  "Joy of Sex The (Europe)/Joy of Sex, The (Europe).cue", 1, ""),
    ("kether",    "KETHER/Kether.cue", 0, ""),
    ("golf",      "L_OPEN_DE_PALM_SPRINGS/GOLF.cue", 0, ""),
    ("link",      "Link-The-Faces-of-Evil-(France)/Link - The Faces of Evil (France).cue", 0, ""),
    ("montypython","Live Without Monty Python (Europe)/Live Without Monty Python (Europe).cue", 0, "5800:578,345"),
    ("myst",      "MYST/Myst (Europe).cue", 0, ""),
    ("maddog",    "Mad Dog McCree (France)/Mad Dog McCree (France).cue", 1, ""),
    ("pinball",   "PINBALL/CDI_Pinball_Volume_1.cue", 0, ""),
    ("secretmission","SECRET_MISSION/Secret_Mission.cue", 0, ""),
    ("7thguest",  "THE_7TH_GUEST_Disque1/The_7th_Guest.cue", 1, ""),
    ("7thguest_cd","THE_7TH_GUEST_Disque2/The_7th_Guest_CD_audio.cue", 0, ""),
    ("zelda_wand","Zelda-The-Wand-of-Gamelon-(France)/Zelda - The Wand of Gamelon (France).cue", 0, ""),
    ("zelda_adv", "Zeldas-Adventure-(Europe)/Zelda's Adventure (Europe).cue", 0, ""),
]


def prepare_workdir(exe):
    """Dossier de travail : l'exe, les DLL et ROM/ du paquet portable."""
    w = tempfile.mkdtemp(prefix="banc_cdi_")
    shutil.copy2(exe, os.path.join(w, "cdi205.exe"))
    for f in os.listdir(PORTABLE):
        if f.lower().endswith(".dll"):
            shutil.copy2(os.path.join(PORTABLE, f), w)
    shutil.copytree(os.path.join(PORTABLE, "ROM"), os.path.join(w, "ROM"))
    # reglages fixes : manettes coupees (une manette touchee changerait les
    # images), bords masques, pas de filtre ni de cartouche par defaut
    with open(os.path.join(w, "options.ini"), "w") as f:
        f.write("pads=0\noverscan=1\nfilter=0\naspect=0\nfmv=0\n")
    return w


def run_game(work, jeu, frames, pas, dest):
    name, image, fmv, clicks = jeu
    os.makedirs(dest, exist_ok=True)
    for f in os.listdir(dest):
        os.remove(os.path.join(dest, f))
    for ext in (".bin", ".rtc"):
        if os.path.exists(NVRAM + ext):
            shutil.copy2(NVRAM + ext, os.path.join(dest, "nvram" + ext))
    env = dict(os.environ)
    env.update({
        "EMU_HEADLESS": "1", "EMU_AUDIO": "0", "EMU_SPEED": "0",
        "EMU_NVRAM": os.path.join(dest, "nvram"),
        "EMU_CLICKS": PLAY + (";" + clicks if clicks else ""),
        "EMU_VIDEO_DUMP": os.path.join(dest, "f%05d.bmp"),
        "EMU_VIDEO_DUMP_EVERY": str(pas),
        "EMU_AUDIO_DUMP": os.path.join(dest, "son.wav"),
        "EMU_EXIT_FRAME": str(frames + 1),
        "EMU_FMV": "1" if fmv else "0",
        # horloge figee (2026-01-01) : les jeux tirent leur hasard de l'heure
        "EMU_RTC_BASE": "1767225600",
    })
    t0 = time.time()
    try:
        r = subprocess.run([os.path.join(work, "cdi205.exe"), os.path.join(GAMES, image)], cwd=work, env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
        status = {0: "ok", 2: "plantage"}.get(r.returncode, "code %d" % r.returncode)
    except subprocess.TimeoutExpired:
        status = "trop long"
    # empreintes : chaque image, et le son (le fichier WAV entier, puis supprime)
    sums = {}
    for f in sorted(os.listdir(dest)):
        p = os.path.join(dest, f)
        if f.endswith(".bmp"):
            sums[f] = hashlib.sha1(open(p, "rb").read()).hexdigest()
    wav = os.path.join(dest, "son.wav")
    if os.path.exists(wav):
        data = open(wav, "rb").read()[44:]
        sums["son"] = "%s %d" % (hashlib.sha1(data).hexdigest(), len(data) // 4)
        os.remove(wav)
    for f in ("nvram.bin", "nvram.rtc"):
        if os.path.exists(os.path.join(dest, f)):
            os.remove(os.path.join(dest, f))
    with open(os.path.join(dest, "empreintes.txt"), "w") as fh:
        for k in sorted(sums):
            fh.write("%s %s\n" % (k, sums[k]))
    return name, status, sums, time.time() - t0


def run_one(exe, jeu, frames, pas, dest):
    """Un dossier de travail par jeu : en parallele, deux emulateurs lisant
    et ecrivant le meme options.ini divergeaient de temps en temps."""
    work = prepare_workdir(exe)
    try:
        return run_game(work, jeu, frames, pas, dest)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def load_sums(d):
    p = os.path.join(d, "empreintes.txt")
    if not os.path.exists(p):
        return None
    out = {}
    for line in open(p):
        k, v = line.rstrip("\n").split(" ", 1)
        out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser(description="Banc de non-regression CD-i 205")
    ap.add_argument("--reference", action="store_true", help="enregistrer la reference")
    ap.add_argument("--exe", default=os.path.join(ROOT, "src", "cdi_emu", "bin", "cdi_emu.exe"))
    ap.add_argument("--jeux", default="", help="noms courts separes par des virgules")
    ap.add_argument("-j", type=int, default=2, help="lancements en parallele")
    ap.add_argument("--frames", type=int, default=12000)
    ap.add_argument("--pas", type=int, default=2000)
    ap.add_argument("--liste", action="store_true")
    a = ap.parse_args()

    if a.liste:
        for name, image, fmv, _ in JEUX:
            present = "ok " if os.path.exists(os.path.join(GAMES, image)) else "-- "
            print(present, "%-14s" % name, image, "(FMV)" if fmv else "")
        return 0
    want = [x.strip() for x in a.jeux.split(",") if x.strip()]
    if not os.path.exists(NVRAM + ".bin"):
        print("Attention : pas de", NVRAM + ".bin (NVRAM de depart) : NVRAM vierge")
    jeux = [j for j in JEUX if (not want or any(w in j[0] for w in want)) and os.path.exists(os.path.join(GAMES, j[1]))]
    if not jeux:
        print("Aucun jeu trouve dans", GAMES)
        return 1
    if not os.path.exists(a.exe):
        print("Executable introuvable :", a.exe)
        return 1

    base = os.path.join(OUT, "reference" if a.reference else "courant")
    print("%s : %d jeux, frames 0..%d (une image toutes les %d), %d en parallele"
          % ("Reference" if a.reference else "Comparaison", len(jeux), a.frames, a.pas, a.j))
    t0 = time.time()
    try:
        with ThreadPoolExecutor(max_workers=max(1, a.j)) as pool:
            res = list(pool.map(lambda j: run_one(a.exe, j, a.frames, a.pas, os.path.join(base, j[0])), jeux))
    finally:
        pass

    bad = 0
    for name, status, sums, dt in res:
        if a.reference:
            print("  %-14s %-9s %3d images  %5.0f s" % (name, status, len([k for k in sums if k.endswith(".bmp")]), dt))
            continue
        ref = load_sums(os.path.join(OUT, "reference", name))
        if ref is None:
            print("  %-14s pas de reference" % name)
            continue
        diffs = [k for k in sorted(set(ref) | set(sums)) if ref.get(k) != sums.get(k)]
        if status != "ok":
            diffs.insert(0, "arret : " + status)
        if diffs:
            bad += 1
            print("  %-14s DIFFERENT : %s" % (name, ", ".join(diffs[:8]) + (" ..." if len(diffs) > 8 else "")))
            # copie cote a cote des images qui different, pour les regarder
            dd = os.path.join(OUT, "differences", name)
            os.makedirs(dd, exist_ok=True)
            for k in diffs:
                if k.endswith(".bmp"):
                    for side, d in (("reference", os.path.join(OUT, "reference", name)), ("nouveau", os.path.join(base, name))):
                        src = os.path.join(d, k)
                        if os.path.exists(src):
                            shutil.copy2(src, os.path.join(dd, k[:-4] + "_" + side + ".bmp"))
        else:
            print("  %-14s identique (%d images + son)" % (name, len(sums) - ("son" in sums)))
    print("Termine en %.0f s." % (time.time() - t0))
    if not a.reference:
        print("Aucune difference." if not bad else
              "%d jeu(x) different(s) : images dans %s" % (bad, os.path.join(OUT, "differences")))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

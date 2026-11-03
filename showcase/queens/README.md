# Queens

Le casse-tête des reines, écrit en Sosu. Le plateau est découpé en autant de régions colorées qu'il a de lignes : il faut y poser une reine par ligne, par colonne et par région, sans que deux reines se touchent, même par un coin.

![Queens](screenshot.png)

## Utilisation

- Boutons **5** à **10** : la longueur du côté (une nouvelle partie commence).
- **New game** : une nouvelle grille. **Clear** : vide le plateau. **Solution** : affiche la solution.
- Clic gauche sur une case : croix (note « pas de reine ici »), puis reine, puis case vide.
- Clic droit : pose ou retire une croix.
- Clavier : `N` nouvelle partie, `C` vider, `S` solution, `Échap` quitter.
- Le chronomètre démarre au premier clic et s'arrête quand la grille est résolue ou que la solution est affichée.
- Les reines qui se gênent sont en rouge.

## Les grilles

Chaque grille n'a qu'**une seule solution**. Le générateur (`src/board.sosu`) place des reines au hasard, fait grandir une région autour de chacune, puis déplace des cases d'une région à l'autre tant que cela ne crée pas de solutions en plus, jusqu'à n'en garder qu'une (comptée par un retour sur trace).

## Fichiers

- `src/board.sosu` : les règles, sans fenêtre (génération, solveur, conflits, victoire).
- `src/main.sosu` : la fenêtre, les boutons, le dessin.
- `src/checks.sosu` : les tests (`sosuc test .`).
- `assets/` : Noto Sans et Font Awesome Free, sous licence SIL OFL (`OFL.txt`, `FontAwesome-LICENSE.txt`).

```
sosuc test .
sosuc run .
.\build\queens.exe --snapshot out.png
```

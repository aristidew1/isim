# Suivi du fork

Ce dépôt est un fork de [kolabs-dev/isim](https://github.com/kolabs-dev/isim). Il sert à compiler et à lancer sous Linux VieLift, une application SwiftUI qui embarque un cœur Kotlin/Native.

## Branches et remotes

| Élément | Valeur |
|---|---|
| Base | `v0.36.0` (`a9d8a1b8`) |
| Branche de travail | `vielift` |
| Remote `origin` | `https://github.com/kolabs-dev/isim` (dépôt d'origine, lecture seule) |
| Remote `fork` | `git@github.com:aristidew1/isim.git` (ce fork) |

La branche `vielift` suit `fork/vielift` : un simple `git push` envoie sur le fork.

Pour récupérer une nouvelle version du dépôt d'origine :

```sh
git fetch origin --tags
git rebase v0.XX.0        # ou : git merge origin/main
git push --force-with-lease
```

## Ce que le fork ajoute (commit `666f9b2e`)

- **`isim kotlin framework`** : lie un framework Kotlin/Native `iosX64` sous Linux (`isim/tools/isim-kotlin.py`, `IsimKonan.kt`, `isim-kotlin-cfshims.m`). Voir [docs/KOTLIN.md](docs/KOTLIN.md).
- **SwiftUI, API manquantes** :
  - `confirmationDialog(presenting:)`
  - `accessibilityActions`
  - `Binding(_: Binding<V?>)`
  - `UIColor(Color)`
  - `ToolbarContent` avec `body`
  - `Button` avec titre `String`, `systemImage` et `role`
- **SwiftUI, comportement et mise en page** :
  - `.task` et `onAppear` ne sont plus lancés pour les onglets cachés d'un `TabView`.
  - `.background { … .ignoresSafeArea() }` s'étend désormais.
  - `lineSpacing` est appliqué.
  - Un `Spacer` enveloppé ne s'étire plus en travers.
  - Les piles ne sont plus replacées avec une hauteur finie.
  - L'espacement par défaut est corrigé à côté des `Spacer`.
  - La largeur des `TextField` est corrigée.
  - Changer l'élément d'un `fullScreenCover(item:)` ne vide plus la pile.
  - Le fond et la poignée des feuilles `.large` sont corrigés.
- **CoreText** :
  - pont `UIFont` ↔ `CTFont` pour Swift ;
  - axes des polices variables transmis à Pango ;
  - `kCTFontVariationAttribute` ;
  - `SFNTLayoutTypes.h`.
- **CoreFoundation / Foundation** : `CFStringGetBytes` et fonctions voisines. Les sous-classes de `NSNumber`, comme les nombres en boîte de Kotlin, sont maintenant lues par leur valeur.
- **libSystem** : `getsegmentdata`, `pthread_override_qos_class_*`, variantes `$DARWIN_EXTSN` (`fopen`, `realpath`).
- **Performances** :
  - cache de `sizeThatFits` par passe ;
  - environnement en boîtes ;
  - traits UIKit linéaires ;
  - cache de mesure du texte (`host.c`) ;
  - PNG en compression 1 ;
  - flou des ombres plus rapide.

## Construction

Il faut LLVM 22, SDL3 et Swift 6.2.4. Exporter `PATH`, `PKG_CONFIG_PATH`, `LD_LIBRARY_PATH` et `CMAKE_PREFIX_PATH` vers LLVM et SDL3, ainsi que `ISIM_SWIFT_TOOLCHAIN` vers la toolchain Swift, puis lancer :

```sh
isim/build.py [cibles]
```

Les produits sont écrits dans `isim/out`. `out/bin/isim` a besoin des bibliothèques SDL3 dans `LD_LIBRARY_PATH`. Un changement interne de SwiftUI recompile environ 99 overlays, soit une dizaine de minutes : mieux vaut grouper les corrections.

## État et reste à faire

- [ ] **Relancer la suite de tests d'isim** ([docs/TESTING.md](docs/TESTING.md)) : elle n'a pas tourné depuis ces changements.
- [ ] Proposer au dépôt d'origine les correctifs génériques, une PR par sujet, une fois les tests au vert.
- [ ] Animations : l'arbre SwiftUI est entièrement recalculé à chaque image, ce qui prend plusieurs secondes par image. Les animations sont donc coupées pour l'instant (`ISIM_ANIMATIONS=0`).
- [ ] Environ 40 % de processeur au repos.
- [ ] Sans animations, on mesure 70 à 150 ms par image au repos et 1,5 à 3 s après un changement d'écran.

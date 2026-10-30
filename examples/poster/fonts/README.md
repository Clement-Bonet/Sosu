# Fonts of the poster

The poster looks for these files here (Google Fonts, SIL Open Font License 1.1):

- `Montserrat-ExtraBold.ttf`: the logo
- `Montserrat-Bold.ttf`: the titles
- `JetBrainsMono-Regular.ttf`: the code window

Download the families (Google Fonts, or the JetBrains Mono release) and copy the **static** TTF
files (the `static/` folder of the archives, not the variable fonts such as `Montserrat[wght].ttf`)
here. Keep the `OFL.txt` of each family next to them.

A file that is missing is replaced by `packages/render/fonts/NotoSans-Regular.ttf` (drawn bolder
for the logo and the titles), and the program says so when it starts.

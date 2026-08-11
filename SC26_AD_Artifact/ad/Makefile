PAPER=ordered_chaos_ad
TECTONIC?=tectonic

$(PAPER).pdf: $(PAPER).tex IEEEtran.cls sc26repro.sty tagging.sty
	$(TECTONIC) $(PAPER).tex

clean:
	rm -f *.aux *.fls *.bbl *.blg *.log *.out *.fdb* *.synctex* *converted*.pdf #$(PAPER).pdf

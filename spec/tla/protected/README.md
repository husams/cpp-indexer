# Protected invariants

`CidxProtected.tla` contains predicates that are part of the review boundary,
not generated output. Changes require explicit human review and must explain
which safety or trust claim changed. Generated tooling may consume these
predicates but may write only to `../generated/`.

@e2e
Feature: Ordered windowed publication preserves canonical query results
  S-117 publishes bounded windows of consecutive translation-unit ranks while
  preserving the query-visible result of one-TU-at-a-time publication.

  Scenario: The five-TU parallel batch matches sequential publication
    Given the S-117 five-TU batch corpus
    When I index the corpus with five workers and with one worker using profile JSON
    Then every translation unit has byte-identical canonical query output

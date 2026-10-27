eval call DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('rerank_reg_ep', '{"url":"http://127.0.0.1:19130/$case/rerank"}');
--error 4070
select ai_rerank('rerank_reg', 'query', '["a", "bb", "ccc"]');

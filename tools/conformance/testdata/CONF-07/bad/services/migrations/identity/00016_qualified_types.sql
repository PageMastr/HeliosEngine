-- +goose Up
CREATE TABLE svc_identity.hops (id BIGINT PRIMARY KEY, origin pg_catalog.inet NOT NULL);
CREATE DOMAIN svc_identity.edge_addr AS svc_identity.ip_addr;
CREATE TABLE svc_identity.edges (id BIGINT PRIMARY KEY, origin svc_identity.edge_addr);
CREATE DOMAIN svc_identity.net_addr AS "pg_catalog"."cidr";
CREATE TABLE svc_identity.nets (id BIGINT PRIMARY KEY, block svc_identity.net_addr);

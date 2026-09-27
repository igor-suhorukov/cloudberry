--
-- The UDP interconnect for the tests of it, which Cloudberry's cluster sends
-- every Motion's rows by (gp_interconnect_type, udpifc): the port's default
-- is tcp, and the sessions of the tests from here to icudp_off take udpifc.
--
ALTER DATABASE regression SET gp.interconnect_type = udpifc;

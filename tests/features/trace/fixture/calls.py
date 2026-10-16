# INV-4: outer calls callee twice, the nested inner once.


def callee():
    return 1


def outer():
    callee()
    callee()

    def inner():
        return callee()

    return inner
